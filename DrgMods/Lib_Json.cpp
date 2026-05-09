#include <emmintrin.h>
#include <cmath>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include "SDK/SDK/Basic.hpp"
template<>
struct std::hash<SDK::FName>
{
    inline size_t operator()(const SDK::FName& n) const noexcept
    {
        return std::hash<uint64_t>{}(
            (uint64_t)(uint32_t)n.ComparisonIndex << 32 | n.Number);
    }
};



#include "Lib_Json.h"
#include "Lib_GameHooks.h"
#include "Lib_ObjectFactory.h"

#include "SDK/SDK/JSONType_structs.hpp"
#include "SDK/SDK/JSON_classes.hpp"
#include "SDK/SDK/JSONValue_classes.hpp"
#include "SDK/SDK/JSON_parameters.hpp"

// —————————————————————————————————————————————————————————————————————————————
// Internal State & Raw UE Structures
// —————————————————————————————————————————————————————————————————————————————

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
        UC::FString          Key;
        SDK::UJSONValue_C* Val;
        int32_t              HashNextId;
        int32_t              HashIndex;
    };

    struct RawBitArray {
        uint32_t  InlineData[4];
        uint32_t* SecondaryData;
        int32_t   NumBits;
        int32_t   MaxBits;
    };

    struct RawSparseArray {
        MapSlot* DataPtr;
        int32_t     NumElements;
        int32_t     MaxElements;
        RawBitArray AllocationFlags;
        int32_t     FirstFreeIndex;
        int32_t     NumFreeIndices;
    };

    struct RawHashAlloc {
        int32_t  InlineData;
        int32_t  _pad;
        int32_t* SecondaryData;
    };

    struct RawMap {
        RawSparseArray Elements;
        RawHashAlloc   Hash;
        int32_t        HashSize;
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
        UObject* outer_;
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
        Parser(const wchar_t* data, int32_t len, UObject* outer) : start_(data), cur_(data), end_(data + len), outer_(outer) { scratch_.reserve(256); }

        SDK::UJSONValue_C* NewNode() {
            auto* node = ObjectFactory::NewObject<SDK::UJSONValue_C>(outer_);
            if (!node) ok_ = false;
            return node;
        }

        SDK::UJSONValue_C* ParseValue() {
            SkipWS();
            if (cur_ >= end_) { ok_ = false; return nullptr; }
            switch (*cur_) {
            case L'{': return ParseObject();
            case L'[': return ParseArray();
            case L'"': {
                auto sv = ParseStringRaw();
                auto* node = NewNode(); if (!node) return nullptr;
                node->Type = JSONType::String;
                node->String = MakeFString(sv.data(), (int32_t)sv.size());
                return node;
            }
            case L't': if (end_ - cur_ >= 4 && wcsncmp(cur_, L"true", 4) == 0) { cur_ += 4; auto* n = NewNode(); if (!n) return nullptr; n->Type = JSONType::Boolean; n->Boolean = true; return n; } break;
            case L'f': if (end_ - cur_ >= 5 && wcsncmp(cur_, L"false", 5) == 0) { cur_ += 5; auto* n = NewNode(); if (!n) return nullptr; n->Type = JSONType::Boolean; n->Boolean = false; return n; } break;
            case L'n': if (end_ - cur_ >= 4 && wcsncmp(cur_, L"null", 4) == 0) { cur_ += 4; auto* n = NewNode(); if (!n) return nullptr; n->Type = JSONType::Null; return n; } break;
            }
            return ParseNumber();
        }

        SDK::UJSONValue_C* ParseObject() {
            Require(L'{');
            if (!ok_) return nullptr;
            auto* node = NewNode(); if (!node) return nullptr;
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
            if (hs <= 1) { m.Hash.InlineData = -1; m.Hash.SecondaryData = nullptr; buckets = &m.Hash.InlineData; }
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
                slot.Val = ParseValue();
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

        SDK::UJSONValue_C* ParseArray() {
            Require(L'[');
            if (!ok_) return nullptr;
            auto* node = NewNode(); if (!node) return nullptr;
            node->Type = JSONType::Array;
            const int32_t N = CountTopLevelItems(L']');
            auto& arr = reinterpret_cast<RawTArray<SDK::UJSONValue_C*>&>(node->Array);
            if (N == 0) { Require(L']'); return node; }

            arr.Data = static_cast<SDK::UJSONValue_C**>(UEAlloc(N * sizeof(void*)));
            arr.Max = N;
            do {
                auto* v = ParseValue();
                if (!ok_) break;
                arr.Data[arr.Num++] = v;
            } while (Eat(L','));
            Require(L']');
            return node;
        }

        SDK::UJSONValue_C* ParseNumber() {
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
            auto* node = NewNode(); if (!node) return nullptr;
            node->Type = JSONType::Number; node->Number = val;
            return node;
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

    static uint8_t* StepOne(FFrame* frame) {
        uint8_t op = *frame->Code++;
        switch (op) {
        case 0x00: case 0x01: case 0x48: {
            auto* prop = *reinterpret_cast<FProperty**>(frame->Code);
            frame->Code += 8;
            return frame->Locals + prop->Offset;
        }
        case 0x17: return reinterpret_cast<uint8_t*>(&frame->Object);
        case 0x38: return nullptr;
        case 0x16: --frame->Code; return nullptr;
        default: return nullptr;
        }
    }

    static void FromStringExec(UObject* Ctx, FFrame* frame, void* Result) {
        if (!g_Active) { if (Result) *reinterpret_cast<void**>(Result) = nullptr; return; }
        UC::FString* pInput = nullptr;
        UObject* outer = nullptr;
        bool* pSuccess = nullptr;
        SDK::UJSONValue_C** pReturn = reinterpret_cast<SDK::UJSONValue_C**>(Result);
        SDK::UJSONValue_C** retSlot = nullptr;

        if (frame->Node == g_Fn) {
            auto* p = reinterpret_cast<SDK::Params::JSON_C_FromString*>(frame->Locals);
            pInput = &p->Input; outer = p->Outer_0; pSuccess = &p->success;
            if (!pReturn) pReturn = &p->Return;
        }
        else {
            pInput = reinterpret_cast<UC::FString*>(StepOne(frame));
            auto* raw1 = StepOne(frame); outer = raw1 ? *reinterpret_cast<UObject**>(raw1) : nullptr;
            StepOne(frame); // WorldContext
            pSuccess = reinterpret_cast<bool*>(StepOne(frame));
            retSlot = reinterpret_cast<SDK::UJSONValue_C**>(StepOne(frame));
            if (!pReturn) pReturn = retSlot;
            if (*frame->Code == 0x16) ++frame->Code;
        }

        auto WriteResult = [&](SDK::UJSONValue_C* val, bool ok) {
            if (pSuccess) *pSuccess = ok;
            if (pReturn)  *pReturn = val;
            if (retSlot && retSlot != pReturn) *retSlot = val;
            };

        if (!pInput || pInput->Num() <= 1) { WriteResult(nullptr, false); return; }
        JsonImpl::Parser parser(pInput->CStr(), pInput->Num() - 1, outer);
        auto* root = parser.ParseValue();
        if (parser.Ok() && root) WriteResult(root, true);
        else WriteResult(nullptr, false);
    }

    static void OnFromStringPE(UObject* Ctx, UFunction* Fn, void* Parms) {
        auto* p = reinterpret_cast<SDK::Params::JSON_C_FromString*>(Parms);
        if (p->Input.Num() <= 1) { p->success = false; p->Return = nullptr; return; }
        JsonImpl::Parser parser(p->Input.CStr(), p->Input.Num() - 1, p->Outer_0);
        auto* root = parser.ParseValue();
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
        g_Fn->FunctionFlags = g_OrigFlags;
        if (g_OrigExec) { g_Fn->ExecFunction = g_OrigExec; g_OrigExec = nullptr; }
        auto myExec = &FromStringExec;
        for (auto* fn : GObjectsOf<SDK::UFunction>()) {
            if (fn->ExecFunction == myExec) {
                if (g_OrigExec) { fn->ExecFunction = g_OrigExec; fn->FunctionFlags = g_OrigFlags; }
                else { fn->FunctionFlags &= ~(uint32_t)SDK::EFunctionFlags::Native; }
            }
        }
    }
}