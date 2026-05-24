#include "Lib_Utils.h"
#include <algorithm>
#include <chrono>
#include <thread>

using namespace SDK;  // file-local; no math types used in this TU

// =========================================================================
// Lib_Forward.h forward-decl implementations
// =========================================================================

bool    IsValid     (const UObject* Obj)   { return Kismet::IsValid(Obj); }
bool    IsValidClass(const UClass* Class)  { return Kismet::IsValidClass(const_cast<UClass*>(Class)); }
UWorld* GetWorld    ()                     { return UWorld::GetWorld(); }

// =========================================================================
// SubclassCache
// =========================================================================

bool SubclassCache::IsSubclassOf(const UClass* derived, const UClass* base)
{
    if (!derived || !base) return false;
    if (derived == base)   return true;
    PairKey key{ derived, base };
    { std::shared_lock lock(mutex_); if (auto it = cache_.find(key); it != cache_.end()) return it->second; }
    bool result = derived->IsSubclassOf(base);
    { std::unique_lock lock(mutex_); cache_.emplace(key, result); }
    return result;
}

void SubclassCache::Clear()
{
    std::unique_lock lock(mutex_);
    cache_.clear();
}

// =========================================================================
// Safe parsers
// =========================================================================

int64_t  SafeStoll (const std::string& s) noexcept { if (s.empty()) return 0;   try { return std::stoll(s);  } catch (...) { return 0; } }
uint64_t SafeStoull(const std::string& s) noexcept { if (s.empty()) return 0;   try { return std::stoull(s); } catch (...) { return 0; } }
float    SafeStof  (const std::string& s) noexcept { if (s.empty()) return 0.f; try { return std::stof(s);  } catch (...) { return 0.f; } }
double   SafeStod  (const std::string& s) noexcept { if (s.empty()) return 0.0; try { return std::stod(s);  } catch (...) { return 0.0; } }

// =========================================================================
// String / FString
// =========================================================================

std::wstring ToWide   (std::string_view in)  { return StringLib::ToWide(in); }
FString      ToFString(std::string_view str) { return FString(StringLib::ToWide(str).c_str()); }

// =========================================================================
// Core helpers
// =========================================================================

std::string GetDisplayName(UObject* Obj) { return Kismet::GetDisplayName(Obj).ToString(); }

bool IsInActiveWorld(UObject* obj)
{
    UWorld* world = UWorld::GetWorld();
    if (!world) return true;
    for (UObject* outer = obj->Outer; outer; outer = outer->Outer)
        if (outer == world) return true;
    return false;
}

void     SleepNow (uint64_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds{ ms }); }
uint64_t GetTimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Exec(std::string cmd)
{
    Kismet::ExecuteConsoleCommand(GetWorld(), FString(StringLib::ToWide(cmd).c_str()), nullptr);
}

bool NearlyEqual(double a, double b, double epsilon)
{
    return std::fabs(a - b) <= epsilon * (std::max)({ 1.0, std::fabs(a), std::fabs(b) });
}

// =========================================================================
// Player helpers
// =========================================================================

APlayerController* GetLocalController()
{
    UWorld* World = GetWorld();
    if (!World) return nullptr;
    UGameInstance* GameInstance = World->OwningGameInstance;
    if (!GameInstance || !GameInstance->LocalPlayers.IsValidIndex(0)) return nullptr;
    ULocalPlayer* LocalPlayer = GameInstance->LocalPlayers[0];
    return LocalPlayer ? LocalPlayer->PlayerController : nullptr;
}

APlayerCharacter* GetLocalPlayer()
{
    APlayerController* Controller = GetLocalController();
    if (!Controller) return nullptr;
    return ObjectCast::Cast<APlayerCharacter>(Controller->Pawn);
}

// =========================================================================
// Misc non-template helpers
// =========================================================================

std::string ObjToStr(const UObject* Obj)
{
    if (!Obj || !IsValid(Obj)) return "None";
    return Obj->GetName();
}

std::string parse_quoted(std::string_view input)
{
    std::string out;
    out.reserve(input.size());

    bool in_quotes  = false;
    bool escape     = false;
    bool found_quote = false;

    for (size_t i = 0; i < input.size(); ++i)
    {
        char c = input[i];

        if (!in_quotes)
        {
            if (c == '"') { in_quotes = true; found_quote = true; }
            continue;
        }

        if (escape)
        {
            switch (c)
            {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case 'n':  out.push_back('\n'); break;
            case 't':  out.push_back('\t'); break;
            default:   out.push_back(c);    break;
            }
            escape = false;
            continue;
        }

        if (c == '\\') { escape = true; continue; }
        if (c == '"')  { return out; }
        out.push_back(c);
    }

    if (!found_quote) return std::string(input);
    return out;
}

void* FindPattern(const wchar_t* moduleName, std::string_view pattern)
{
    HMODULE mod = GetModuleHandleW(moduleName);
    if (!mod) return nullptr;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);

    std::vector<uint8_t> pat;
    std::vector<bool>    mask;
    const char* s = pattern.data();
    const char* e = s + pattern.size();
    while (s < e)
    {
        while (s < e && *s == ' ') ++s;
        if (s >= e) break;
        if (s[0] == '?' && s + 1 < e && s[1] == '?')
        {
            pat.push_back(0x00); mask.push_back(false); s += 2;
        }
        else
        {
            char hex[3] = { s[0], s[1], '\0' };
            pat.push_back(static_cast<uint8_t>(std::strtoul(hex, nullptr, 16)));
            mask.push_back(true);
            s += 2;
        }
    }

    const size_t len = pat.size();
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (!(sec->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)))
            continue;

        const uint8_t* begin = reinterpret_cast<uint8_t*>(mod) + sec->VirtualAddress;
        const uint8_t* last  = begin + sec->Misc.VirtualSize - len;
        for (const uint8_t* p = begin; p <= last; ++p)
        {
            bool match = true;
            for (size_t j = 0; j < len && match; ++j)
                if (mask[j] && p[j] != pat[j]) match = false;
            if (match) return const_cast<uint8_t*>(p);
        }
    }
    return nullptr;
}
