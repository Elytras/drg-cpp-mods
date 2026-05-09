#pragma once
// Lib_Json.h — Fast JSON parser replacing UJSON_C::FromString via ExecFunction patch.
//
// Strategy: intercept ProcessEvent(FromString, Parms) with SkipOriginal.
// CallFunction pre-evaluates all args from the calling frame's bytecode into a Parms
// buffer before calling ProcessEvent, so Parms is a fully populated JSON_C_FromString
// struct.  We write success/Return back into Parms; CallFunction copies them to the
// caller's local vars after ProcessEvent returns.
#include "Lib_GameHooks.h"
#include "Lib_ObjectFactory.h"
#include <emmintrin.h>  // SSE2 — always available on x64
#include <cmath>
#include "SDK/SDK/JSONType_structs.hpp"
#include "SDK/SDK/JSON_classes.hpp"
#include "SDK/SDK/JSONValue_classes.hpp"
#include "SDK/SDK/JSON_parameters.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Named JSONType constants
// ─────────────────────────────────────────────────────────────────────────────

// EJSONType enum mapping.
// Verified at runtime via ProbeTypeEnum(): NewEnumeratorX == underlying byte value.
//   NewEnumerator2=0 (String), NewEnumerator3=1 (Number), NewEnumerator1=2 (Object),
//   NewEnumerator0=3 (Array),  NewEnumerator4=4 (Boolean), NewEnumerator5=5 (Null)
namespace JSONType {
    using T = SDK::EJSONType;
    inline constexpr T Null    = T::NewEnumerator5;  // = 5
    inline constexpr T String  = T::NewEnumerator2;  // = 0
    inline constexpr T Number  = T::NewEnumerator3;  // = 1
    inline constexpr T Boolean = T::NewEnumerator4;  // = 4
    inline constexpr T Array   = T::NewEnumerator0;  // = 3
    inline constexpr T Object  = T::NewEnumerator1;  // = 2
}

namespace JsonImpl {

static constexpr uintptr_t kCRCTableRVA = 0x629c5c0;

inline const uint32_t* CRCTable()
{
    static const uint32_t* t = nullptr;
    if (!t) {
        HMODULE mod = GetModuleHandleW(L"FSD-Win64-Shipping.exe");
        t = reinterpret_cast<const uint32_t*>(
            reinterpret_cast<uint8_t*>(mod) + kCRCTableRVA);
        spdlog::debug("[JsonImpl] CRCTable @ {:p}", reinterpret_cast<const void*>(t));
    }
    return t;
}

// Matches FCrc::Strihash_DEPRECATED<wchar_t>(int32 DataLen, const wchar_t* Data).
// Verified from disassembly @ 140859440:
//   hash starts at 0 (no ~init), returns without inversion.
//   ASCII ToUpper per char, then 2 CRC rounds (low byte, high byte).
//   Iterates DataLen chars — null terminator is NOT included.
inline uint32_t StrihashDeprecated(const wchar_t* s, int32_t len)
{
    const uint32_t* T = CRCTable();
    uint32_t hash = 0;
    for (int32_t i = 0; i < len; ++i) {
        wchar_t ch = s[i];
        if (ch >= L'a' && ch <= L'z') ch -= 0x20; // ASCII ToUpper
        hash = (hash >> 8) ^ T[(hash ^ ch) & 0xFF];
        ch >>= 8;
        hash = (hash >> 8) ^ T[(hash ^ ch) & 0xFF];
    }
    return hash;
}

inline void* UEAlloc(size_t n)
{
    return SDK::UnrealAllocator::Get()->Malloc(static_cast<uint64_t>(n));
}

inline UC::FString MakeFString(const wchar_t* s, int32_t len)
{
    int32_t cap = len + 1;
    auto* buf = static_cast<wchar_t*>(UEAlloc(cap * sizeof(wchar_t)));
    memcpy(buf, s, len * sizeof(wchar_t));
    buf[len] = L'\0';
    return UC::FString(buf, cap, cap);
}

inline UC::FString MakeFString(std::wstring_view sv)
{
    return MakeFString(sv.data(), static_cast<int32_t>(sv.size()));
}

struct MapSlot {
    UC::FString          Key;
    SDK::UJSONValue_C*   Val;
    int32_t              HashNextId;
    int32_t              HashIndex;
};
static_assert(sizeof(MapSlot) == 0x20);

struct RawBitArray {
    uint32_t  InlineData[4];
    uint32_t* SecondaryData;
    int32_t   NumBits;
    int32_t   MaxBits;
};
static_assert(sizeof(RawBitArray) == 0x20);

struct RawSparseArray {
    MapSlot*    DataPtr;
    int32_t     NumElements;
    int32_t     MaxElements;
    RawBitArray AllocationFlags;
    int32_t     FirstFreeIndex;
    int32_t     NumFreeIndices;
};
static_assert(sizeof(RawSparseArray) == 0x38);
static_assert(offsetof(RawSparseArray, AllocationFlags) == 0x10);
static_assert(offsetof(RawSparseArray, FirstFreeIndex)  == 0x30);

struct RawHashAlloc {
    int32_t  InlineData;
    int32_t  _pad;
    int32_t* SecondaryData;
};
static_assert(sizeof(RawHashAlloc) == 0x10);

struct RawMap {
    RawSparseArray Elements;
    RawHashAlloc   Hash;
    int32_t        HashSize;
    int32_t        _pad;
};
static_assert(sizeof(RawMap) == 0x50);
static_assert(offsetof(RawMap, Hash)     == 0x38);
static_assert(offsetof(RawMap, HashSize) == 0x48);
static_assert(sizeof(UC::TMap<UC::FString, SDK::UJSONValue_C*>) == 0x50);

template<typename T>
struct RawTArray { T* Data; int32_t Num; int32_t Max; };

class Parser {
    const wchar_t* start_;
    const wchar_t* cur_;
    const wchar_t* end_;
    UObject*       outer_;
    bool           ok_ = true;
    std::wstring   scratch_; // reused across all ParseStringRaw calls

    // ── whitespace ────────────────────────────────────────────────────────────

    void SkipWS() {
        // SSE2: 8 wchar_t per iteration.
        // cmpeq_epi16 → 0xFFFF/0x0000 per lane; movemask_epi8 → 2 bits per wchar_t.
        const __m128i vSp  = _mm_set1_epi16(0x0020);
        const __m128i vTab = _mm_set1_epi16(0x0009);
        const __m128i vCR  = _mm_set1_epi16(0x000D);
        const __m128i vLF  = _mm_set1_epi16(0x000A);
        while (cur_ + 8 <= end_) {
            __m128i v  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cur_));
            __m128i ws = _mm_or_si128(
                _mm_or_si128(_mm_cmpeq_epi16(v, vSp),  _mm_cmpeq_epi16(v, vTab)),
                _mm_or_si128(_mm_cmpeq_epi16(v, vCR),  _mm_cmpeq_epi16(v, vLF)));
            int mask = _mm_movemask_epi8(ws);
            if (mask == 0xFFFF) { cur_ += 8; continue; }
            if (mask == 0)      return;
            // Mixed: advance past leading whitespace wchar_ts.
            // Each wchar_t occupies 2 consecutive bits; both set means whitespace.
            for (int i = 0; i < 8; ++i) {
                if (((mask >> (2 * i)) & 3) != 3) return;
                ++cur_;
            }
        }
        while (cur_ < end_ &&
               (*cur_ == L' ' || *cur_ == L'\t' || *cur_ == L'\r' || *cur_ == L'\n'))
            ++cur_;
    }

    bool Peek(wchar_t c) { SkipWS(); return cur_ < end_ && *cur_ == c; }
    bool Eat(wchar_t c)  { if (!Peek(c)) return false; ++cur_; return true; }
    void Require(wchar_t c) { if (!Eat(c)) ok_ = false; }

    // ── lookahead ─────────────────────────────────────────────────────────────

    // Counts comma-separated items at depth 0 inside {} or [].
    // cur_ must point to the first character after the opening bracket.
    // Returns 0 for empty containers; otherwise 1 + number of top-level commas.
    int32_t CountTopLevelItems(wchar_t close) const {
        const wchar_t* p = cur_;
        while (p < end_ && (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')) ++p;
        if (p >= end_ || *p == close) return 0;
        int     depth = 0;
        int32_t n     = 1;
        bool    inStr = false;
        for (; p < end_; ++p) {
            if (inStr) {
                if      (*p == L'\\') ++p; // skip escaped char
                else if (*p == L'"')  inStr = false;
                continue;
            }
            switch (*p) {
            case L'"':                    inStr = true; break;
            case L'{': case L'[':         ++depth;      break;
            case L'}': case L']':
                if (depth == 0) return n;
                --depth; break;
            case L',': if (depth == 0) ++n; break;
            }
        }
        return n;
    }

    // ── string ────────────────────────────────────────────────────────────────

    // Returns a view into scratch_. Valid until the next ParseStringRaw() call;
    // caller must copy (via MakeFString) before invoking ParseStringRaw() again.
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
                        if      (h >= L'0' && h <= L'9') v |= h - L'0';
                        else if (h >= L'a' && h <= L'f') v |= h - L'a' + 10;
                        else if (h >= L'A' && h <= L'F') v |= h - L'A' + 10;
                        else { ok_ = false; }
                    }
                    scratch_ += v;
                    break;
                }
                default: ok_ = false; break;
                }
            } else {
                scratch_ += *cur_++;
            }
        }
        Require(L'"');
        return std::wstring_view(scratch_);
    }

    // ── nodes ─────────────────────────────────────────────────────────────────

    SDK::UJSONValue_C* NewNode() {
        auto* node = ObjectFactory::NewObject<SDK::UJSONValue_C>(outer_);
        if (!node) ok_ = false;
        return node;
    }

    // ── object ────────────────────────────────────────────────────────────────

    SDK::UJSONValue_C* ParseObject() {
        Require(L'{');
        if (!ok_) return nullptr;
        auto* node = NewNode();
        if (!node) return nullptr;
        node->Type = JSONType::Object;

        const int32_t N = CountTopLevelItems(L'}');
        if (N == 0) { Require(L'}'); return node; }

        auto& m  = reinterpret_cast<RawMap&>(node->Object);
        auto& sa = m.Elements;
        sa.DataPtr        = static_cast<MapSlot*>(UEAlloc(N * sizeof(MapSlot)));
        sa.MaxElements    = N;
        sa.FirstFreeIndex = -1;
        sa.NumFreeIndices = 0;

        // Hash table — sized to next power-of-2 >= N
        int32_t hs = 1;
        while (hs < N) hs <<= 1;
        m.HashSize = hs;
        int32_t* buckets;
        if (hs <= 1) {
            m.Hash.InlineData    = -1;
            m.Hash.SecondaryData = nullptr;
            buckets = &m.Hash.InlineData;
        } else {
            m.Hash.SecondaryData = static_cast<int32_t*>(UEAlloc(hs * sizeof(int32_t)));
            m.Hash.InlineData    = -1;
            buckets = m.Hash.SecondaryData;
            for (int32_t i = 0; i < hs; ++i) buckets[i] = -1;
        }
        const uint32_t hmask = static_cast<uint32_t>(hs - 1);

        // Parse entries directly into slots; build hash table inline — no vector intermediary.
        // scratch_ must be copied to slot.Key before ParseValue() can overwrite it.
        int32_t filled = 0;
        do {
            auto key = ParseStringRaw();
            if (!ok_) break;
            auto&    slot = sa.DataPtr[filled];
            uint32_t h    = StrihashDeprecated(key.data(), static_cast<int32_t>(key.size()));
            slot.Key      = MakeFString(key.data(), static_cast<int32_t>(key.size()));
            Require(L':');
            slot.Val = ParseValue();
            if (!ok_) { ++filled; break; }
            int32_t buck    = static_cast<int32_t>(h & hmask);
            slot.HashIndex  = buck;
            slot.HashNextId = buckets[buck];
            buckets[buck]   = filled++;
        } while (Eat(L','));
        Require(L'}');

        sa.NumElements = filled;

        // AllocationFlags — mark the 'filled' contiguous valid slots
        auto& bf = sa.AllocationFlags;
        auto SetBits = [](uint32_t* words, int32_t n) {
            for (int32_t w = 0; w < (n + 31) / 32; ++w)
                words[w] = (w < n / 32) ? 0xFFFFFFFFu : (1u << (n & 31)) - 1u;
        };
        if (filled <= 128) {
            memset(bf.InlineData, 0, sizeof(bf.InlineData));
            SetBits(bf.InlineData, filled);
            bf.SecondaryData = nullptr;
            bf.MaxBits       = 128;
        } else {
            int32_t words    = (filled + 31) / 32;
            bf.SecondaryData = static_cast<uint32_t*>(UEAlloc(words * sizeof(uint32_t)));
            memset(bf.SecondaryData, 0, words * sizeof(uint32_t));
            SetBits(bf.SecondaryData, filled);
            bf.MaxBits       = words * 32;
        }
        bf.NumBits = filled;

        return node;
    }

    // ── array ─────────────────────────────────────────────────────────────────

    SDK::UJSONValue_C* ParseArray() {
        Require(L'[');
        if (!ok_) return nullptr;
        auto* node = NewNode();
        if (!node) return nullptr;
        node->Type = JSONType::Array;

        const int32_t N = CountTopLevelItems(L']');
        if (N == 0) { Require(L']'); return node; }

        auto& arr = reinterpret_cast<RawTArray<SDK::UJSONValue_C*>&>(node->Array);
        arr.Data  = static_cast<SDK::UJSONValue_C**>(UEAlloc(N * sizeof(void*)));
        arr.Num   = 0;
        arr.Max   = N;

        do {
            auto* v = ParseValue();
            if (!ok_) break;
            arr.Data[arr.Num++] = v;
        } while (Eat(L','));
        Require(L']');
        return node;
    }

    // ── number ────────────────────────────────────────────────────────────────

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
                val  += static_cast<float>(*cur_++ - L'0') * scale;
                scale *= 0.1f;
            }
        }

        if (cur_ < end_ && (*cur_ == L'e' || *cur_ == L'E')) {
            ++cur_;
            const bool negE = (cur_ < end_ && *cur_ == L'-');
            if (cur_ < end_ && (*cur_ == L'+' || *cur_ == L'-')) ++cur_;
            int exp = 0;
            while (cur_ < end_ && *cur_ >= L'0' && *cur_ <= L'9')
                exp = exp * 10 + (*cur_++ - L'0');
            static constexpr float kPow10[21] = {
                1e0f,1e1f,1e2f,1e3f,1e4f,1e5f,1e6f,1e7f,1e8f,1e9f,1e10f,
                1e11f,1e12f,1e13f,1e14f,1e15f,1e16f,1e17f,1e18f,1e19f,1e20f };
            float mult = (exp < 21) ? kPow10[exp] : std::pow(10.0f, static_cast<float>(exp));
            val = negE ? val / mult : val * mult;
        }

        if (neg) val = -val;

        auto* node = NewNode();
        if (!node) return nullptr;
        node->Type   = JSONType::Number;
        node->Number = val;
        return node;
    }

public:
    Parser(const wchar_t* data, int32_t len, UObject* outer)
        : start_(data), cur_(data), end_(data + len), outer_(outer)
    {
        scratch_.reserve(256);
    }

    SDK::UJSONValue_C* ParseValue() {
        SkipWS();
        if (cur_ >= end_) { ok_ = false; return nullptr; }
        switch (*cur_) {
        case L'{': return ParseObject();
        case L'[': return ParseArray();
        case L'"': {
            auto  sv   = ParseStringRaw();
            auto* node = NewNode();
            if (!node) return nullptr;
            node->Type   = JSONType::String;
            node->String = MakeFString(sv.data(), static_cast<int32_t>(sv.size()));
            return node;
        }
        case L't':
            if (end_ - cur_ >= 4 && wcsncmp(cur_, L"true", 4) == 0) {
                cur_ += 4;
                auto* node = NewNode(); if (!node) return nullptr;
                node->Type = JSONType::Boolean; node->Boolean = true;
                return node;
            }
            ok_ = false; return nullptr;
        case L'f':
            if (end_ - cur_ >= 5 && wcsncmp(cur_, L"false", 5) == 0) {
                cur_ += 5;
                auto* node = NewNode(); if (!node) return nullptr;
                node->Type = JSONType::Boolean; node->Boolean = false;
                return node;
            }
            ok_ = false; return nullptr;
        case L'n':
            if (end_ - cur_ >= 4 && wcsncmp(cur_, L"null", 4) == 0) {
                cur_ += 4;
                auto* node = NewNode(); if (!node) return nullptr;
                node->Type = JSONType::Null;
                return node;
            }
            ok_ = false; return nullptr;
        default:
            return ParseNumber();
        }
    }

    bool    Ok()  const { return ok_; }
    int32_t Pos() const { return static_cast<int32_t>(cur_ - start_); }
};

} // namespace JsonImpl

// ─────────────────────────────────────────────────────────────────────────────
// Hook
//
// Strategy: ExecFunction patch.
//
// UJSON_C::FromString is a pure static Blueprint function library function.
// Callers compile it as EX_CallMath, which calls fn->ExecFunction directly —
// ProcessEvent is never involved, so ProcessEvent hooks do nothing.
//
// We set FUNC_Native on the UFunction and replace ExecFunction with our stub.
// EX_CallMath then calls our stub with the calling frame; Code points at the
// un-evaluated param bytecodes that we consume manually.
//
// Call path detection via frame->Node:
//   EX_CallMath path  → Node is the CALLING function (≠ g_Fn);
//                        consume params from frame->Code bytecodes.
//   ProcessEvent path → Node == g_Fn;
//                        params are pre-evaluated in frame->Locals.
//
// Param order in bytecode (JSON_C_FromString):
//   Input (FString, in), Outer_0 (UObject*, in), __WorldContext (UObject*, in),
//   success (bool, out), EX_EndFunctionParms.
//   Return value → written to void* Result (RESULT_PARAM).
// ─────────────────────────────────────────────────────────────────────────────

namespace JsonHook {

static SDK::UFunction*                g_Fn          = nullptr;
static SDK::UFunction::FNativeFuncPtr g_OrigExec    = nullptr; // null if fn had no native impl
static uint32_t                       g_OrigFlags   = 0;
static GameHooks::CallbackHandle      g_PEHandle    = 0;
static bool                           g_Active      = false;   // false after Teardown, true after Setup
static bool                           g_ProbeNeeded = false;

static void DumpJson(const SDK::UJSONValue_C* node, int depth, std::string& out)
{
    if (!node) { out += "null"; return; }
    auto type = (int)(uint8_t)node->Type;
    if (type == (int)(uint8_t)JSONType::Null)    { out += "null"; return; }
    if (type == (int)(uint8_t)JSONType::Boolean) { out += node->Boolean ? "true" : "false"; return; }
    if (type == (int)(uint8_t)JSONType::Number) {
        char buf[32]; snprintf(buf, sizeof(buf), "%g", node->Number);
        out += buf; return;
    }
    if (type == (int)(uint8_t)JSONType::String) {
        out += '"';
        if (node->String.CStr()) {
            for (int i = 0; i < node->String.Num() - 1; ++i) {
                wchar_t c = node->String.CStr()[i];
                out += (c < 128) ? (char)c : '?';
            }
        }
        out += '"'; return;
    }
    if (type == (int)(uint8_t)JSONType::Array) {
        out += '[';
        auto& arr = reinterpret_cast<const JsonImpl::RawTArray<SDK::UJSONValue_C*>&>(node->Array);
        for (int i = 0; i < arr.Num; ++i) {
            if (i) out += ',';
            DumpJson(arr.Data[i], depth + 1, out);
            if (out.size() > 2000) { out += "..."; return; }
        }
        out += ']'; return;
    }
    if (type == (int)(uint8_t)JSONType::Object) {
        out += '{';
        auto& m = reinterpret_cast<const JsonImpl::RawMap&>(node->Object);
        bool first = true;
        for (int i = 0; i < m.Elements.NumElements; ++i) {
            auto& slot = m.Elements.DataPtr[i];
            if (!slot.Key.CStr()) continue;
            if (!first) out += ',';
            first = false;
            out += '"';
            for (int j = 0; j < slot.Key.Num() - 1; ++j) {
                wchar_t c = slot.Key.CStr()[j]; out += (c < 128) ? (char)c : '?';
            }
            out += "\":";
            DumpJson(slot.Val, depth + 1, out);
            if (out.size() > 2000) { out += "..."; return; }
        }
        out += '}'; return;
    }
    out += "?type=" + std::to_string(type);
}

// Evaluate one bytecode expression in frame, advance frame->Code past it.
// Returns pointer to the value location (in-param) or write target (out-param).
// Returns nullptr for EX_Nothing / EX_EndFunctionParms (latter is un-consumed).
static uint8_t* StepOne(FFrame* frame)
{
    uint8_t op = *frame->Code++;
    switch (op) {
    case 0x00: // EX_LocalVariable
    case 0x01: // EX_InstanceVariable
    {
        auto* prop = *reinterpret_cast<FProperty**>(frame->Code);
        frame->Code += 8;
        return frame->Locals + prop->Offset;
    }
    case 0x48: // EX_LocalOutVariable
    {
        auto* prop = *reinterpret_cast<FProperty**>(frame->Code);
        frame->Code += 8;
        return frame->Locals + prop->Offset;
    }
    case 0x17: // EX_Self — value is the calling UObject*
        return reinterpret_cast<uint8_t*>(&frame->Object);
    case 0x38: // EX_Nothing — null / absent param
        return nullptr;
    case 0x16: // EX_EndFunctionParms — don't consume, signal stop
        --frame->Code;
        return nullptr;
    default:
        spdlog::error("[JsonHook] StepOne: unhandled opcode 0x{:02X}", op);
        return nullptr;
    }
}

// Probe each setter on a freshly created UJSONValue_C node and log the resulting
// Type integer. Call this once on the game thread to map EJSONType enumerators.
static void ProbeTypeEnum()
{
    auto* node = ObjectFactory::NewObject<SDK::UJSONValue_C>(nullptr);
    if (!node) {
        spdlog::error("[JsonHook] ProbeTypeEnum: NewObject failed");
        return;
    }
    auto* cls = SDK::UJSONValue_C::StaticClass();
    if (!cls) {
        spdlog::error("[JsonHook] ProbeTypeEnum: StaticClass failed");
        return;
    }
    spdlog::info("[JsonHook] ProbeTypeEnum: initial Type = {}", (int)(uint8_t)node->Type);

    auto call = [&](const char* name, auto fill) {
        auto* fn = cls->GetFunction("JSONValue_C", name);
        if (!fn) { spdlog::warn("[JsonHook] ProbeTypeEnum: {} not found", name); return; }
        std::vector<uint8_t> parms(256, 0); // generous fixed buffer; probe is diagnostic-only
        fill(parms);
        node->ProcessEvent(fn, parms.data());
        spdlog::info("[JsonHook] ProbeTypeEnum: after {} Type = {}", name, (int)(uint8_t)node->Type);
    };

    call("SetString", [](std::vector<uint8_t>& b) {
        auto s = JsonImpl::MakeFString(L"x", 1);
        memcpy(b.data(), &s, sizeof(UC::FString));
    });
    call("SetNumber", [](std::vector<uint8_t>& b) {
        float v = 1.0f; memcpy(b.data(), &v, sizeof(float));
    });
    call("SetBoolean", [](std::vector<uint8_t>& b) {
        bool v = true; memcpy(b.data(), &v, sizeof(bool));
    });
    call("SetArray",  [](std::vector<uint8_t>&) {}); // zero-init = empty TArray
    call("SetObject", [](std::vector<uint8_t>&) {}); // zero-init = empty TMap
}

static void FromStringExec(UObject* /*Ctx*/, FFrame* frame, void* Result)
{
    // Stub may remain installed after Teardown when g_OrigExec was null (no
    // native implementation to fall back to).  Return failure gracefully.
    if (!g_Active) {
        if (Result) *reinterpret_cast<void**>(Result) = nullptr;
        return;
    }
    if (g_ProbeNeeded) { g_ProbeNeeded = false; ProbeTypeEnum(); }

    FString*            pInput   = nullptr;
    UObject*            outer    = nullptr;
    bool*               pSuccess = nullptr;
    // RESULT_PARAM: where EX_CallMath picks up the return value.
    SDK::UJSONValue_C** pReturn  = reinterpret_cast<SDK::UJSONValue_C**>(Result);
    // retSlot: the EX_LocalOutVariable address the BP local var lives at.
    // The game reads the return value from here, not from RESULT_PARAM.
    // Must write to BOTH so either call path works.
    SDK::UJSONValue_C** retSlot  = nullptr;

    if (frame->Node == g_Fn) {
        // ProcessEvent/Invoke path — params are in frame->Locals
        auto* p = reinterpret_cast<SDK::Params::JSON_C_FromString*>(frame->Locals);
        pInput   = &p->Input;
        outer    = p->Outer_0;
        pSuccess = &p->success;
        if (!pReturn) pReturn = &p->Return;
    } else {
        // Direct-call path (EX_CallMath / ProcessLocalScriptFunction):
        // frame->Code points at un-evaluated param bytecodes.
        pInput = reinterpret_cast<FString*>(StepOne(frame)); // Input (in)
        auto* raw1 = StepOne(frame); // Outer_0 (in)
        outer = raw1 ? *reinterpret_cast<UObject**>(raw1) : nullptr;
        StepOne(frame); // __WorldContext (in)
        pSuccess = reinterpret_cast<bool*>(StepOne(frame)); // success (out)
        retSlot  = reinterpret_cast<SDK::UJSONValue_C**>(StepOne(frame)); // Return (out)
        if (!pReturn) pReturn = retSlot;
        if (*frame->Code == 0x16) ++frame->Code; // EX_EndFunctionParms
    }

    auto WriteResult = [&](SDK::UJSONValue_C* val, bool ok) {
        if (pSuccess) *pSuccess = ok;
        if (pReturn)  *pReturn  = val;
        // Also write to the BP local-var slot — the game reads from here.
        if (retSlot && retSlot != pReturn) *retSlot = val;
    };

    if (!pInput || pInput->Num() <= 1) {
        WriteResult(nullptr, false);
        return;
    }

    JsonImpl::Parser parser(pInput->CStr(), pInput->Num() - 1, outer);
    auto* root = parser.ParseValue();

    if (parser.Ok() && root) {
        WriteResult(root, true);
    } else {
        spdlog::warn("[JsonHook] parse failed at char {}", parser.Pos());
        WriteResult(nullptr, false);
    }
}

// ProcessEvent path (non-pure, non-native call): Parms is pre-evaluated JSON_C_FromString.
static void OnFromStringPE(UObject* /*Ctx*/, UFunction* /*Fn*/, void* Parms)
{
    auto* p = reinterpret_cast<SDK::Params::JSON_C_FromString*>(Parms);
    spdlog::debug("[JsonHook][PE] enter Input.Num={}", p->Input.Num());

    if (p->Input.Num() <= 1) { p->success = false; p->Return = nullptr; return; }

    JsonImpl::Parser parser(p->Input.CStr(), p->Input.Num() - 1, p->Outer_0);
    auto* root = parser.ParseValue();
    if (parser.Ok() && root) { p->success = true;  p->Return = root; }
    else                      { p->success = false; p->Return = nullptr; }
}

inline void Setup()
{
    if (g_Active) return;

    if (!g_Fn)
        g_Fn = SDK::UJSON_C::StaticClass()->GetFunction("JSON_C", "FromString");
    if (!g_Fn) { spdlog::error("[JsonHook] FromString UFunction not found"); return; }

    spdlog::info("[JsonHook] FromString @ {:p}  flags=0x{:08X}  origExec={:p}",
        static_cast<void*>(g_Fn), g_Fn->FunctionFlags,
        reinterpret_cast<void*>(g_Fn->ExecFunction));

    // Path A — ExecFunction patch.
    // If stub is already installed (re-Setup after Teardown with null OrigExec),
    // skip saving state — g_OrigFlags/g_OrigExec are already the true originals.
    if (g_Fn->ExecFunction != &FromStringExec) {
        g_OrigFlags        = g_Fn->FunctionFlags;
        g_OrigExec         = g_Fn->ExecFunction;
        g_Fn->ExecFunction = &FromStringExec;
    }
    g_Fn->FunctionFlags |= static_cast<uint32_t>(SDK::EFunctionFlags::Native);
    g_Active = true;
    spdlog::info("[JsonHook] ExecFunction patched, FUNC_Native set (origExec was {})",
        g_OrigExec ? "non-null" : "null — stub will remain on teardown");

    // Path B — ProcessEvent hook (catches CallFunction→ProcessEvent for non-native calls).
    if (g_PEHandle == 0)
        g_PEHandle = GameHooks::OnProcessEventByFunction(g_Fn, OnFromStringPE,
            GameHooks::ExecutionTiming::Before, GameHooks::ExecutionMode::SkipOriginal);
    spdlog::info("[JsonHook] ProcessEvent hook by-function installed (handle={})", g_PEHandle);
}

inline void Teardown()
{
    if (!g_Fn) return;

    g_Active = false; // make stub return failure immediately if still called

    // Remove ProcessEvent callback.
    if (g_PEHandle != 0) {
        GameHooks::RemoveHook(g_PEHandle);
        g_PEHandle = 0;
    }

    // Restore flags exactly.
    g_Fn->FunctionFlags = g_OrigFlags;

    // Only restore ExecFunction if the original was non-null.
    // If g_OrigExec was null the function had no native impl; putting null back
    // causes EX_CallMath callers to call through a null pointer and crash.
    // Leaving our stub in place is safe — g_Active=false makes it return failure.
    if (g_OrigExec) {
        g_Fn->ExecFunction = g_OrigExec;
        g_OrigExec  = nullptr;
        g_OrigFlags = 0;
        spdlog::info("[JsonHook] ExecFunction restored to original");
    } else {
        spdlog::info("[JsonHook] ExecFunction kept (stub) — no original to restore");
    }

    // ── GObjects sweep ───────────────────────────────────────────────────────
    // Blueprint class regeneration can create duplicate UFunction objects that
    // inherit our patched ExecFunction pointer.  Teardown only restored g_Fn
    // above; any copies still point into our (now-unloaded) DLL stub and will
    // crash when Blueprint VM later calls them via EX_LocalFinalFunction.
    // Sweep every live UObject and patch out any remaining references to our stub.
    {
        const auto myExec       = &FromStringExec;
        const auto* fnClass     = SDK::UFunction::StaticClass();
        int32_t swept           = 0;
        const int32_t numObjs   = SDK::UObject::GObjects->Num();
        for (auto* fn : GObjectsOf<UFunction>()) {
            if (fn->ExecFunction != myExec) continue;
            // Restore ExecFunction.  If original was null we leave the stub
            // installed (same logic as above) so EX_CallMath doesn't null-deref;
            // g_Active=false already makes the stub return failure gracefully.
            if (g_OrigExec) {
                fn->ExecFunction  = g_OrigExec;
                fn->FunctionFlags = g_OrigFlags;
            }
            // If g_OrigExec is null: leave stub, flags were already cleared on g_Fn above;
            // clear FUNC_Native on this copy too so ProcessEvent won't try ExecFunction.
            else {
                fn->FunctionFlags &= ~static_cast<uint32_t>(SDK::EFunctionFlags::Native);
            }

            ++swept;
            spdlog::warn("[JsonHook] Swept extra UFunction @ {:p} ({})",
                static_cast<void*>(fn), fn->GetName());
        }

        if (swept > 0)
            spdlog::info("[JsonHook] Swept {} extra UFunction instance(s)", swept);
        else
            spdlog::debug("[JsonHook] GObjects sweep: no extra instances found");
    }
}

} // namespace JsonHook