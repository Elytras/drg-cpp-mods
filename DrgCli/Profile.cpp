#include "Profile.h"
#include "Injector.h"  // GetProcId
#include <cwctype>
#include <cstring>

const Profile kDrgProfile = {
    GameId::DRG,
    L"DRG",
    L"FSD-Win64-Shipping.exe",
    L"DrgMods.dll",
    "FSD-Win64-Shipping.exe",

    L"Local\\DRG_Logs",
    L"Local\\DRG_InjLog",
    L"Local\\DRG_Commands",
    L"Local\\DRG_Response",
    L"Local\\DRG_Meta",

    L"Local\\DRG_LogReady",
    L"Local\\DRG_InjLogReady",
    L"Local\\DRG_CmdReady",
    L"Local\\DRG_ResponseReady",
    L"Local\\DRG_Shutdown",
    L"Local\\DRG_ShutdownDone",
    L"Local\\DRG_DllReady",
};

const Profile kRcProfile = {
    GameId::RC,
    L"RC",
    L"RogueCore-Win64-Shipping.exe",
    L"RcMods.dll",
    "RogueCore-Win64-Shipping.exe",

    L"Local\\RC_Logs",
    L"Local\\RC_InjLog",
    L"Local\\RC_Commands",
    L"Local\\RC_Response",
    L"Local\\RC_Meta",

    L"Local\\RC_LogReady",
    L"Local\\RC_InjLogReady",
    L"Local\\RC_CmdReady",
    L"Local\\RC_ResponseReady",
    L"Local\\RC_Shutdown",
    L"Local\\RC_ShutdownDone",
    L"Local\\RC_DllReady",
};

const Profile* g_Profile = &kDrgProfile;

static bool ICaseEqW(const wchar_t* a, const wchar_t* b)
{
    while (*a && *b)
    {
        if (towlower(*a) != towlower(*b)) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

const Profile* ResolveProfile(const wchar_t* token)
{
    if (!token) return nullptr;
    if (ICaseEqW(token, L"drg") || ICaseEqW(token, L"fsd")) return &kDrgProfile;
    if (ICaseEqW(token, L"rc")  || ICaseEqW(token, L"roguecore")) return &kRcProfile;
    return nullptr;
}

const Profile* InitProfile(const Profile* requested)
{
    if (requested)
    {
        g_Profile = requested;
        return g_Profile;
    }

    // Auto-detect: prefer whichever game is running. If both, prefer DRG
    // (historical default). If neither, default to DRG.
    if (GetProcId(kDrgProfile.targetProcess)) { g_Profile = &kDrgProfile; return g_Profile; }
    if (GetProcId(kRcProfile.targetProcess))  { g_Profile = &kRcProfile;  return g_Profile; }
    g_Profile = &kDrgProfile;
    return g_Profile;
}
