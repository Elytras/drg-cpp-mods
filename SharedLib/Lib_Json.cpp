#include <emmintrin.h>
#include <cmath>
#include <vector>
#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include "Lib_Json.h"
#include "Lib_GameHooks.h"
#include "Lib_ObjectFactory.h"
#include "Lib_Utils.h"
#include "Lib_Print.h"
#include "Lib_PropertyInspector.h"

#include "SDK/SDK/JSONType_structs.hpp"
#include "SDK/SDK/JSON_classes.hpp"
#include "SDK/SDK/JSONValue_classes.hpp"
#include "SDK/SDK/JSON_parameters.hpp"

using namespace SDK;  // file-local; no math types used in this TU

// ─────────────────────────────────────────────────────────────────────────────
// Internal State & Raw UE Structures
// ─────────────────────────────────────────────────────────────────────────────

namespace JSONType {
    using T = SDK::EJSONType;
    inline constexpr T Null = T::NewEnumerator5;  // 5
    inline constexpr T String = T::NewEnumerator2;  // 0
    inline constexpr T Number = T::NewEnumerator3;  // 1
    inline constexpr T Boolean = T::NewEnumerator4;  // 4
    inline constexpr T Array = T::NewEnumerator0;  // 3
    inline constexpr T Object = T::NewEnumerator1;  // 2
}

namespace JsonImpl {
    static constexpr uintptr_t kCRCTableRVA = 0x629c5c0;

    struct MapSlot {
        UC::FString             Key;
        SDK::UJSONValue_C*      Val;
        int32_t                 HashNextId;
        int32_t                 HashIndex;
    };

    struct RawBitArray {
        uint32_t  InlineData[4];
        uint32_t* SecondaryData;
        int32_t   NumBits;
        int32_t   MaxBits;
    };

    struct RawSparseArray {
        MapSlot*    DataPtr;
        int32_t     NumElements;
        int32_t     MaxElements;
        RawBitArray AllocationFlags;
        int32_t     FirstFreeIndex;
        int32_t     NumFreeIndices;
    };

    struct RawHashAlloc {
        int32_t  InlineData;
    private:
        int32_t  _pad;
    public:
        int32_t* SecondaryData;
    };

    struct RawMap {
        RawSparseArray Elements;
        RawHashAlloc   Hash;
        int32_t        HashSize;
    private:
        int32_t        _pad;
    };

    template<typename T>
    struct RawTArray { T* Data; int32_t Num; int32_t Max; };

    inline const uint32_t* CRCTable() {
        static const uint32_t* t = nullptr;
        if (!t) {
            HMODULE mod = GetModuleHandleW(L"FSD-Win64-Shipping.exe");
            if (mod) t = reinterpret_cast<const uint32_t*>(reinterpret_cast<uint8_t*>(mod) + kCRCTableRVA);
        }
        return t;
    }

    inline uint32_t StrihashDeprecated(const wchar_t* s, int32_t len) {
        const uint32_t* T = CRCTable();
        if (!T) return 0;
        uint32_t hash = 0;
        for (int32_t i = 0; i < len; ++i) {
            wchar_t ch = s[i];
            if (ch >= L'a' && ch <= L'z') ch -= 0x20;
            hash = (hash >> 8) ^ T[(hash ^ ch) & 0xFF];
            ch >>= 8;
            hash = (hash >> 8) ^ T[(hash ^ ch) & 0xFF];
        }
        return hash;
    }

    inline void* UEAlloc(size_t n) {
        return SDK::UnrealAllocator::Get()->Malloc(static_cast<uint64_t>(n));
    }

    inline UC::FString MakeFString(const wchar_t* s, int32_t len) {
        int32_t cap = len + 1;
        auto* buf = static_cast<wchar_t*>(UEAlloc(cap * sizeof(wchar_t)));
        memcpy(buf, s, len * sizeof(wchar_t));
        buf[len] = L'\0';
        return UC::FString(buf, cap, cap);
    }

    class Parser {
        const wchar_t* start_, * cur_, * end_;
        UObject* initialOuter_;
        bool ok_ = true;
        std::wstring scratch_;

        void SkipWS() {
            const __m128i vSp = _mm_set1_epi16(0x0020);
            const __m128i vTab = _mm_set1_epi16(0x0009);
            const __m128i vCR = _mm_set1_epi16(0x000D);
            const __m128i vLF = _mm_set1_epi16(0x000A);
            while (cur_ + 8 <= end_) {
                __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cur_));
                __m128i ws = _mm_or_si128(
                    _mm_or_si128(_mm_cmpeq_epi16(v, vSp), _mm_cmpeq_epi16(v, vTab)),
                    _mm_or_si128(_mm_cmpeq_epi16(v, vCR), _mm_cmpeq_epi16(v, vLF)));
                int mask = _mm_movemask_epi8(ws);
                if (mask == 0xFFFF) { cur_ += 8; continue; }
                if (mask == 0) return;
                for (int i = 0; i < 8; ++i) {
                    if (((mask >> (2 * i)) & 3) != 3) return;
                    ++cur_;
                }
            }
            while (cur_ < end_ && (*cur_ == L' ' || *cur_ == L'\t' || *cur_ == L'\r' || *cur_ == L'\n'))
                ++cur_;
        }

        bool Peek(wchar_t c) { SkipWS(); return cur_ < end_ && *cur_ == c; }
        bool Eat(wchar_t c) { if (!Peek(c)) return false; ++cur_; return true; }
        void Require(wchar_t c) { if (!Eat(c)) ok_ = false; }

        int32_t CountTopLevelItems(wchar_t close) const {
            const wchar_t* p = cur_;
            while (p < end_ && (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')) ++p;
            if (p >= end_ || *p == close) return 0;
            int depth = 0, n = 1; bool inStr = false;
            for (; p < end_; ++p) {
                if (inStr) {
                    if (*p == L'\\') ++p;
                    else if (*p == L'"') inStr = false;
                    continue;
                }
                switch (*p) {
                case L'"': inStr = true; break;
                case L'{': case L'[': ++depth; break;
                case L'}': case L']': if (depth == 0) return n; --depth; break;
                case L',': if (depth == 0) ++n; break;
                }
            }
            return n;
        }

        std::wstring_view ParseStringRaw() {
            scratch_.clear();
            Require(L'"');
            while (cur_ < end_ && *cur_ != L'"') {
                if (*cur_ == L'\\') {
                    if (++cur_ >= end_) { ok_ = false; break; }
                    switch (*cur_++) {
                    case L'"':  scratch_ += L'"';  break;
                    case L'\\': scratch_ += L'\\'; break;
                    case L'/':  scratch_ += L'/';  break;
                    case L'b':  scratch_ += L'\b'; break;
                    case L'f':  scratch_ += L'\f'; break;
                    case L'n':  scratch_ += L'\n'; break;
                    case L'r':  scratch_ += L'\r'; break;
                    case L't':  scratch_ += L'\t'; break;
                    case L'u': {
                        if (cur_ + 4 > end_) { ok_ = false; break; }
                        wchar_t v = 0;
                        for (int i = 0; i < 4; ++i) {
                            wchar_t h = *cur_++;
                            v <<= 4;
                            if (h >= L'0' && h <= L'9') v |= h - L'0';
                            else if (h >= L'a' && h <= L'f') v |= h - L'a' + 10;
                            else if (h >= L'A' && h <= L'F') v |= h - L'A' + 10;
                            else ok_ = false;
                        }
                        scratch_ += v; break;
                    }
                    default: ok_ = false; break;
                    }
                }
                else scratch_ += *cur_++;
            }
            Require(L'"');
            return scratch_;
        }

    public:
        Parser(const wchar_t* data, int32_t len, UObject* outer)
            : start_(data), cur_(data), end_(data + len), initialOuter_(outer) {
            scratch_.reserve(256);
        }

        SDK::UJSONValue_C* NewNode(UObject* currentOuter) {
            auto* node = ObjectFactory::NewObject<SDK::UJSONValue_C>(currentOuter);
            if (!node) ok_ = false;
            return node;
        }

        SDK::UJSONValue_C* ParseValue(UObject* currentOuter) {
            SkipWS();
            if (cur_ >= end_) { ok_ = false; return nullptr; }
            switch (*cur_) {
            case L'{': return ParseObject(currentOuter);
            case L'[': return ParseArray(currentOuter);
            case L'"': {
                auto sv = ParseStringRaw();
                auto* node = NewNode(currentOuter);
                if (!node) return nullptr;
                node->Type = JSONType::String;
                node->String = MakeFString(sv.data(), (int32_t)sv.size());
                return node;
            }
            case L't':
                if (end_ - cur_ >= 4 && wcsncmp(cur_, L"true", 4) == 0) {
                    cur_ += 4;
                    auto* n = NewNode(currentOuter);
                    if (!n) return nullptr;
                    n->Type = JSONType::Boolean;
                    n->Boolean = true;
                    return n;
                }
                break;
            case L'f':
                if (end_ - cur_ >= 5 && wcsncmp(cur_, L"false", 5) == 0) {
                    cur_ += 5;
                    auto* n = NewNode(currentOuter);
                    if (!n) return nullptr;
                    n->Type = JSONType::Boolean;
                    n->Boolean = false;
                    return n;
                }
                break;
            case L'n':
                if (end_ - cur_ >= 4 && wcsncmp(cur_, L"null", 4) == 0) {
                    cur_ += 4;
                    auto* n = NewNode(currentOuter);
                    if (!n) return nullptr;
                    n->Type = JSONType::Null;
                    return n;
                }
                break;
            }
            return ParseNumber(currentOuter);
        }

        SDK::UJSONValue_C* ParseObject(UObject* currentOuter) {
            Require(L'{');
            if (!ok_) return nullptr;

            // Create the object node with the current outer
            auto* node = NewNode(currentOuter);
            if (!node) return nullptr;
            node->Type = JSONType::Object;

            const int32_t N = CountTopLevelItems(L'}');
            if (N == 0) { Require(L'}'); return node; }

            auto& m = reinterpret_cast<RawMap&>(node->Object);
            auto& sa = m.Elements;
            sa.DataPtr = static_cast<MapSlot*>(UEAlloc(N * sizeof(MapSlot)));
            sa.MaxElements = N;
            sa.FirstFreeIndex = -1;
            sa.NumFreeIndices = 0;

            int32_t hs = 1; while (hs < N) hs <<= 1;
            m.HashSize = hs;
            int32_t* buckets;
            if (hs <= 1) {
                m.Hash.InlineData = -1;
                m.Hash.SecondaryData = nullptr;
                buckets = &m.Hash.InlineData;
            }
            else {
                buckets = static_cast<int32_t*>(UEAlloc(hs * sizeof(int32_t)));
                m.Hash.SecondaryData = buckets;
                m.Hash.InlineData = -1;
                for (int i = 0; i < hs; ++i) buckets[i] = -1;
            }

            int32_t filled = 0;
            do {
                auto keyView = ParseStringRaw();
                if (!ok_) break;
                auto& slot = sa.DataPtr[filled];
                uint32_t h = StrihashDeprecated(keyView.data(), (int32_t)keyView.size());
                slot.Key = MakeFString(keyView.data(), (int32_t)keyView.size());

                Require(L':');

                // IMPORTANT: Pass 'node' as the Outer for the child value
                slot.Val = ParseValue(node);

                if (!ok_) { filled++; break; }
                int32_t buck = static_cast<int32_t>(h & (hs - 1));
                slot.HashIndex = buck;
                slot.HashNextId = buckets[buck];
                buckets[buck] = filled++;
            } while (Eat(L','));

            Require(L'}');
            sa.NumElements = filled;

            auto& bf = sa.AllocationFlags;
            auto SetBits = [](uint32_t* words, int32_t n) {
                for (int32_t w = 0; w < (n + 31) / 32; ++w)
                    words[w] = (w < n / 32) ? 0xFFFFFFFFu : (1u << (n & 31)) - 1u;
                };
            if (filled <= 128) {
                memset(bf.InlineData, 0, sizeof(bf.InlineData));
                SetBits(bf.InlineData, filled);
                bf.SecondaryData = nullptr;
                bf.MaxBits = 128;
            }
            else {
                int32_t words = (filled + 31) / 32;
                bf.SecondaryData = static_cast<uint32_t*>(UEAlloc(words * sizeof(uint32_t)));
                memset(bf.SecondaryData, 0, words * sizeof(uint32_t));
                SetBits(bf.SecondaryData, filled);
                bf.MaxBits = words * 32;
            }
            bf.NumBits = filled;
            return node;
        }

        SDK::UJSONValue_C* ParseArray(UObject* currentOuter) {
            Require(L'[');
            if (!ok_) return nullptr;

            // Create the array node with the current outer
            auto* node = NewNode(currentOuter);
            if (!node) return nullptr;
            node->Type = JSONType::Array;

            const int32_t N = CountTopLevelItems(L']');
            auto& arr = reinterpret_cast<RawTArray<SDK::UJSONValue_C*>&>(node->Array);
            if (N == 0) { Require(L']'); return node; }

            arr.Data = static_cast<SDK::UJSONValue_C**>(UEAlloc(N * sizeof(void*)));
            arr.Max = N;
            do {
                // IMPORTANT: Pass 'node' as the Outer for the child element
                auto* v = ParseValue(node);
                if (!ok_) break;
                arr.Data[arr.Num++] = v;
            } while (Eat(L','));

            Require(L']');
            return node;
        }

        SDK::UJSONValue_C* ParseNumber(UObject* currentOuter) {
            SkipWS();
            if (cur_ >= end_) { ok_ = false; return nullptr; }
            const bool neg = (*cur_ == L'-');
            if (neg && ++cur_ >= end_) { ok_ = false; return nullptr; }
            if (*cur_ < L'0' || *cur_ > L'9') { ok_ = false; return nullptr; }

            float val = 0.0f;
            while (cur_ < end_ && *cur_ >= L'0' && *cur_ <= L'9')
                val = val * 10.0f + static_cast<float>(*cur_++ - L'0');

            if (cur_ < end_ && *cur_ == L'.') {
                ++cur_;
                float scale = 0.1f;
                while (cur_ < end_ && *cur_ >= L'0' && *cur_ <= L'9') {
                    val += static_cast<float>(*cur_++ - L'0') * scale;
                    scale *= 0.1f;
                }
            }
            if (cur_ < end_ && (*cur_ == L'e' || *cur_ == L'E')) {
                ++cur_;
                const bool negE = (cur_ < end_ && *cur_ == L'-');
                if (cur_ < end_ && (*cur_ == L'+' || *cur_ == L'-')) ++cur_;
                int exp = 0;
                while (cur_ < end_ && *cur_ >= L'0' && *cur_ <= L'9') exp = exp * 10 + (*cur_++ - L'0');
                static constexpr float kPow10[21] = { 1e0f,1e1f,1e2f,1e3f,1e4f,1e5f,1e6f,1e7f,1e8f,1e9f,1e10f,1e11f,1e12f,1e13f,1e14f,1e15f,1e16f,1e17f,1e18f,1e19f,1e20f };
                float mult = (exp < 21) ? kPow10[exp] : std::pow(10.0f, (float)exp);
                val = negE ? val / mult : val * mult;
            }
            if (neg) val = -val;

            auto* node = NewNode(currentOuter);
            if (!node) return nullptr;
            node->Type = JSONType::Number;
            node->Number = val;
            return node;
        }

        // Helper to kick off parsing from the root
        SDK::UJSONValue_C* Parse() {
            return ParseValue(initialOuter_);
        }

        bool Ok() const { return ok_; }
        int32_t Pos() const { return (int32_t)(cur_ - start_); }
    };
}

namespace JsonHook {
    static SDK::UFunction* g_Fn = nullptr;
    static SDK::UFunction::FNativeFuncPtr g_OrigExec = nullptr;
    static uint32_t                       g_OrigFlags = 0;
    static GameHooks::CallbackHandle      g_PEHandle = 0;
    static bool                           g_Active = false;

    static uint8_t* StepOne(FFrame* frame, FProperty *& Out) {
        uint8_t op = *frame->Code++;
        info("OpCode: 0x{:X}", op);
        switch (op) {
        case 0x00: // EX_LocalVariable
        case 0x01: // EX_InstanceVariable
        case 0x48: // EX_Context (Simplified handling)
        {
            auto* prop = *reinterpret_cast<FProperty**>(frame->Code);
            frame->Code += 8;

            // Determine the correct base pointer
            uint8_t* container = nullptr;
            if (op == 0x00) {
                container = frame->Locals;
            }
            else if (op == 0x01) {
                container = reinterpret_cast<uint8_t*>(frame->Object);
            }
            else if (op == 0x48) {
                return nullptr;
            }

            if (!container) return nullptr;

            uint8_t* valuePtr = container + prop->Offset;

            info("PropName: {} | {} | Flags: {}",
                prop->Name.ToString(),
                prop->ClassPrivate->Name.ToString(),
                DumpPropertyFlags(prop->PropertyFlags));

            static const FName ObjectProperty = FName(L"ObjectProperty");
            if (prop->ClassPrivate->Name == ObjectProperty) {
                UObject* obj = *reinterpret_cast<UObject**>(valuePtr);
                if (obj) {
                    __assume(obj);
                    PropertyInspector::Dump(obj);
                }
                else {
                    info(" (Object is null)");
                }
            }

            return valuePtr;
        }
        case 0x17:
        {
            info("Object: {}", frame->Object->GetName());
            return reinterpret_cast<uint8_t*>(&frame->Object);
        }
        case 0x38: return nullptr;
        case 0x16: --frame->Code; return nullptr;
        default: return nullptr;
        }
    }
    static void FromStringExec(UObject* Ctx, FFrame* frame, void* Result) {
        static uint64 totalCount = 0;
        static uint32 sequenceCount = 0;
        static auto lastCallTime = std::chrono::steady_clock::now();

        auto now = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCallTime).count();

        if (elapsedMs > 100) {
            info("------------------- New Sequence (Gap: {}ms) -------------------", elapsedMs);
            sequenceCount = 0;
        }

        lastCallTime = now;
        totalCount++;
        sequenceCount++;

        info("[Call #{}] (Seq: {})", totalCount, sequenceCount);
        // --------------------------

        if (!g_Active) {
            if (Result) *reinterpret_cast<void**>(Result) = nullptr;
            return;
        }

        UC::FString* pInput = nullptr;
        UObject* outer = nullptr;
        bool* pSuccess = nullptr;
        SDK::UJSONValue_C** pReturn = reinterpret_cast<SDK::UJSONValue_C**>(Result);
        SDK::UJSONValue_C** retSlot = nullptr;

        if (frame->Node == g_Fn) {

            info("Node == g_Fn");

            auto* p = reinterpret_cast<SDK::Params::JSON_C_FromString*>(frame->Locals);

            pInput = &p->Input; outer = p->Outer_0; pSuccess = &p->success;

            if (!pReturn) pReturn = (&p->Return);
        }
        else
        {

            FProperty* Out;

            pInput = reinterpret_cast<UC::FString*>(StepOne(frame, Out));

            uint8* raw1 = StepOne(frame, Out);
            outer = raw1 ? *reinterpret_cast<UObject**>(raw1) : nullptr;

            StepOne(frame, Out); // WorldContext

            pSuccess = reinterpret_cast<bool*>(StepOne(frame, Out));
            retSlot = reinterpret_cast<SDK::UJSONValue_C**>(StepOne(frame, Out));

            if (!pReturn) pReturn = retSlot;

            if (*frame->Code == 0x16) ++frame->Code;
        }

        auto WriteResult = [&](SDK::UJSONValue_C* val, bool ok)
            {
            if (pSuccess) *pSuccess = ok;
            if (pReturn)  *pReturn = val;
            if (retSlot && retSlot != pReturn) *retSlot = val;
            };

        if (!pInput || pInput->Num() <= 1)
        {
            WriteResult(nullptr, false);
            return;
        }

        struct LastOuterInfo {
            int32 IndexPrivate;
            FName Name;
        };

        static FString LastString{};
        static UJSONValue_C* LastJson{};
        static LastOuterInfo Info{};

        JsonImpl::Parser parser(pInput->CStr(), pInput->Num() - 1, outer);

        if (!outer || !Info.IndexPrivate || Info.IndexPrivate != outer->Index || Info.Name != outer->Name || LastString != *pInput || !LastJson)
        {
            {
                if (!outer) info("Reparsing because outer invalid");
                else if (!Info.IndexPrivate) info("Reparsing because cached IndexPrivate is invalid");
                else if (Info.IndexPrivate != outer->Index) info("Reparsing because different outer (index)");
                else if (Info.Name != outer->Name) info("Reparsing because different outer (Name)");
                else if (!LastJson) info("Reparsing because LastJson doesn't exist");
                else info("Reparsing becuase string is different. Len Old {}, Len New {}",LastString.Num(),pInput->Num());
            }
            LastJson = parser.Parse();

            Info = { outer->Index, outer->Name };
            if (LastString.GetInnerPtr() != nullptr )
            {
                UnrealAllocator::Get()->Free(LastString.GetInnerPtr());
            }
            LastString = JsonImpl::MakeFString(pInput->CStr(), pInput->Num()-1);

        }
        else {
            info("saved a parse");
        }

        if (parser.Ok() && LastJson) {
            WriteResult(LastJson, true);
        }
        else {
            info("!! Parse Failed at Seq {}", sequenceCount);
            WriteResult(nullptr, false);
        }
    }

    static void OnFromStringPE(UObject* Ctx, UFunction* Fn, void* Parms) {
        auto* p = reinterpret_cast<SDK::Params::JSON_C_FromString*>(Parms);
        if (p->Input.Num() <= 1) { p->success = false; p->Return = nullptr; return; }
        JsonImpl::Parser parser(p->Input.CStr(), p->Input.Num() - 1, p->Outer_0);
        auto* root = parser.Parse();
        p->success = parser.Ok() && root;
        p->Return = p->success ? root : nullptr;
    }

    void Setup() {
        if (g_Active) return;
        if (!g_Fn) g_Fn = SDK::UJSON_C::StaticClass()->GetFunction("JSON_C", "FromString");
        if (!g_Fn) return;
        if (g_Fn->ExecFunction != &FromStringExec) {
            g_OrigFlags = g_Fn->FunctionFlags;
            g_OrigExec = g_Fn->ExecFunction;
            g_Fn->ExecFunction = &FromStringExec;
        }
        g_Fn->FunctionFlags |= (uint32_t)SDK::EFunctionFlags::Native;
        if (g_PEHandle == 0)
            g_PEHandle = GameHooks::OnProcessEventByFunction(g_Fn, OnFromStringPE, GameHooks::ExecutionTiming::Before, GameHooks::ExecutionMode::SkipOriginal);
        g_Active = true;
    }

    void Teardown() {
        if (!g_Fn) return;
        g_Active = false;
        if (g_PEHandle != 0) { GameHooks::RemoveHook(g_PEHandle); g_PEHandle = 0; }

        // Snapshot originals before we clear them — the sweep needs them too.
        const auto savedExec  = g_OrigExec;
        const auto savedFlags = g_OrigFlags;

        g_Fn->FunctionFlags = savedFlags;
        if (savedExec) {
            g_Fn->ExecFunction = savedExec;
            g_OrigExec  = nullptr;
            g_OrigFlags = 0;
        }
        g_Fn = nullptr; // rediscover on next Setup() in case class regenerates

        // Sweep GObjects for duplicate UFunction copies that also point at our stub.
        // Blueprint class regeneration can create copies that inherit the patched
        // ExecFunction; Teardown only restored g_Fn above — copies survive and crash
        // after FreeLibrary when the Blueprint VM calls them.
        const auto myExec = &FromStringExec;
        int32_t swept = 0;
        for (auto* fn : GObjectsOf<SDK::UFunction>()) {
            if (fn->ExecFunction != myExec) continue;
            if (savedExec) {
                // Restore to original thunk + original flags.
                fn->ExecFunction  = savedExec;
                fn->FunctionFlags = savedFlags;
            } else {
                // Original was null (pure BP, no native thunk).
                // Can't restore null — EX_CallMath would crash.
                // Strip FUNC_Native so ProcessEvent won't try to dispatch
                // through ExecFunction; our stub stays but g_Active=false
                // makes it return null gracefully on any EX_CallMath hit.
                fn->FunctionFlags &= ~(uint32_t)SDK::EFunctionFlags::Native;
            }
            ++swept;
            warn("[JsonHook] Swept extra UFunction copy @ {:p}", static_cast<void*>(fn));
        }
        if (swept)
            info("[JsonHook] Swept {} extra UFunction instance(s)", swept);
        else
            debug("[JsonHook] GObjects sweep: no extra copies found");
    }
}
