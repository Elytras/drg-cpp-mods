#pragma once
// Lib_KeyBindings.h — Game/CLI/system-wide keybinding registration.

#include <cstdint>
#include <functional>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Key codes
// ─────────────────────────────────────────────────────────────────────────────

enum class Key : uint16_t
{
    // Digits
    D0 = 0x30, D1, D2, D3, D4, D5, D6, D7, D8, D9,

    // Letters
    A = 0x41, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    // Function keys
    F1  = 0x70, F2,  F3,  F4,  F5,  F6,
    F7,         F8,  F9,  F10, F11, F12,

    // Editing / navigation
    BackSpace = 0x08,
    Tab       = 0x09,
    Return    = 0x0D,
    Escape    = 0x1B,
    Space     = 0x20,
    PageUp    = 0x21,
    PageDown  = 0x22,
    End       = 0x23,
    Home      = 0x24,
    Left      = 0x25,
    Up        = 0x26,
    Right     = 0x27,
    Down      = 0x28,
    Insert    = 0x2D,
    Delete    = 0x2E,

    // Numpad
    Num0      = 0x60, Num1, Num2, Num3, Num4,
    Num5,             Num6, Num7, Num8, Num9,
    NumMul    = 0x6A,
    NumAdd    = 0x6B,
    NumSub    = 0x6D,
    NumDec    = 0x6E,
    NumDiv    = 0x6F,
    NumLock   = 0x90,

    // OEM / typeable punctuation
    Semicolon    = 0xBA,  // ;  (also = on some layouts)
    Equal        = 0xBB,  // =  (also + on some layouts)
    Comma        = 0xBC,  // ,
    Minus        = 0xBD,  // -
    Period       = 0xBE,  // .
    Slash        = 0xBF,  // /
    Grave        = 0xC0,  // `
    LeftBracket  = 0xDB,  // [
    Backslash    = 0xDC,  // backslash
    RightBracket = 0xDD,  // ]
    Quote        = 0xDE,  // '

    // Mouse buttons
    MouseLeft   = 0x01,  // VK_LBUTTON
    MouseRight  = 0x02,  // VK_RBUTTON
    MouseMiddle = 0x04,  // VK_MBUTTON
    MouseX1     = 0x05,  // VK_XBUTTON1
    MouseX2     = 0x06,  // VK_XBUTTON2

    // Modifier keys — groundwork for future primary-key binding support.
    // For now use Mod flags, not these as primary keys.
    Shift  = 0x10,
    Ctrl   = 0x11,
    Alt    = 0x12,
    LShift = 0xA0,
    RShift = 0xA1,
    LCtrl  = 0xA2,
    RCtrl  = 0xA3,
    LAlt   = 0xA4,
    RAlt   = 0xA5,
};

// ─────────────────────────────────────────────────────────────────────────────
//  Modifier flags
// ─────────────────────────────────────────────────────────────────────────────

enum class Mod : uint8_t
{
    None  = 0,
    Ctrl  = 1 << 0,
    Shift = 1 << 1,
    Alt   = 1 << 2,
};

constexpr Mod  operator|(Mod a, Mod b) noexcept { return static_cast<Mod>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }
constexpr Mod  operator&(Mod a, Mod b) noexcept { return static_cast<Mod>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b)); }
constexpr Mod& operator|=(Mod& a, Mod b) noexcept { return a = a | b; }
constexpr bool operator!(Mod m) noexcept { return m == Mod::None; }

// ─────────────────────────────────────────────────────────────────────────────
//  Trigger
// ─────────────────────────────────────────────────────────────────────────────

enum class Trigger : uint8_t
{
    Press   = 1 << 0,  // fires once on key-down
    Release = 1 << 1,  // fires once on key-up
    Held    = 1 << 2,  // fires repeatedly at heldMs intervals while the key is held
};

constexpr Trigger operator|(Trigger a, Trigger b) noexcept { return static_cast<Trigger>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }
constexpr Trigger operator&(Trigger a, Trigger b) noexcept { return static_cast<Trigger>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b)); }
constexpr bool    operator!(Trigger t)            noexcept { return static_cast<uint8_t>(t) == 0; }

// ─────────────────────────────────────────────────────────────────────────────
//  Focus scope
// ─────────────────────────────────────────────────────────────────────────────

enum class Focus : uint8_t
{
    Game,    // binding fires only when the game window has focus
    CLI,     // binding fires only when DrgCli has focus
    Any,     // binding fires when either the game or CLI has focus
    Global,  // binding fires regardless of which window has focus
};

// ─────────────────────────────────────────────────────────────────────────────
//  IsTypeable
//  True if the key produces a visible character when typed.
//  suppress=true is silently ignored for typeable keys in CLI-scope bindings
//  so that e.g. binding 'A' with suppress doesn't prevent typing in the CLI.
// ─────────────────────────────────────────────────────────────────────────────

constexpr bool IsTypeable(Key k) noexcept
{
    auto v = static_cast<uint16_t>(k);
    return v == 0x20                     // Space
        || (v >= 0x30 && v <= 0x39)      // 0–9
        || (v >= 0x41 && v <= 0x5A)      // A–Z
        || (v >= 0xBA && v <= 0xC0)      // ; = , - . / `
        || (v >= 0xDB && v <= 0xDE);     // [ \ ] '
}

// ─────────────────────────────────────────────────────────────────────────────
//  BindingOptions
// ─────────────────────────────────────────────────────────────────────────────

struct BindingOptions
{
    Trigger          trigger  = Trigger::Press;
    Focus            focus    = Focus::Game;
    bool             suppress = false;
    int              heldMs   = 300;          // repeat interval for Trigger::Held
    std::vector<Key> coKeys;                  // extra keys that must be held simultaneously
};

using BindingHandle = size_t;

// ─────────────────────────────────────────────────────────────────────────────
//  KeyBindings API
// ─────────────────────────────────────────────────────────────────────────────

class CommandHandler;

namespace KeyBindings
{
    // Register a binding. Returns a non-zero handle on success.
    // Logs an error and returns 0 if (key, mods, focus) conflicts with an
    // already-registered binding with the same focus scope.
    BindingHandle Register
    (
        Key key,
        Mod mods,
        std::function<void()> callback,
        BindingOptions opts = {}
    );

    // Register a binding to be executed on game thread
    BindingHandle RegisterGameThread
    (
        Key key,
        Mod mods,
        std::function<void()> callback,
        BindingOptions opts = {}
    );

    void Unregister(BindingHandle handle);

    // Install the WH_KEYBOARD_LL hook. Call once from WorkerThread after
    // shared memory is up. Safe to call multiple times (idempotent).
    void Init();

    // Signal the hook thread to exit. Blocks until it does.
    void Shutdown();

    // Inform the keybinding system of the CLI console window handle so
    // Focus::CLI and Focus::Any bindings can check window focus correctly.
    // Called once in Main.cpp after reading MetaBuffer.
    void SetCLIWindow(uintptr_t hwnd);

    // Register bind/unbind/bindings commands into the given handler.
    // Implementations are stubs pending full CLI-bindable support.
    void RegisterCommands(CommandHandler& handler);
}
