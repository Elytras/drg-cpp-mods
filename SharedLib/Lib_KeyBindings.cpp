#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "Lib_KeyBindings.h"
#include "Lib_CommandHandler.h"
#include "Lib_GameHooks.h"

#include <algorithm>
#include <cctype>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <spdlog/spdlog.h>

namespace KeyBindings
{
    // ─────────────────────────────────────────────────────────────────────────
    //  Internal types
    // ─────────────────────────────────────────────────────────────────────────

    struct BindingEntry
    {
        BindingHandle         handle;
        Key                   key;
        Mod                   mods;
        BindingOptions        opts;
        std::function<void()> callback;
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  State (accessed from both the registration thread and the hook thread)
    // ─────────────────────────────────────────────────────────────────────────

    static std::shared_mutex           s_mutex;
    static std::vector<BindingEntry>   s_bindings;
    static size_t                      s_nextHandle = 1;

    static HHOOK                       s_hHook        = nullptr;
    static HHOOK                       s_hMouseHook   = nullptr;
    static DWORD                       s_hookThreadId = 0;
    static HANDLE                      s_hookThread   = nullptr;
    static std::atomic<uintptr_t>      s_cliHwnd{ 0 };

    // ─────────────────────────────────────────────────────────────────────────
    //  Held-key tracking — only touched on the hook thread, no locking needed
    // ─────────────────────────────────────────────────────────────────────────

    struct HeldEntry { Mod mods; DWORD lastFireMs; };
    static std::unordered_map<int, HeldEntry> s_held;

    // ─────────────────────────────────────────────────────────────────────────
    //  Lib ↔ system mappings — single source of truth for all enum↔VK lookups
    // ─────────────────────────────────────────────────────────────────────────

    struct ModEntry { Mod m; int vk; const char* name; };
    static constexpr ModEntry kModTable[] = {
        { Mod::Ctrl,  VK_CONTROL, "Ctrl"  },
        { Mod::Shift, VK_SHIFT,   "Shift" },
        { Mod::Alt,   VK_MENU,    "Alt"   },
    };

    struct KeyEntry { Key k; int vk; const char* name; };
    static constexpr KeyEntry kKeyTable[] = {
        // Letters (VK = ASCII uppercase, no named VK_x constant)
        { Key::A,0x41,"A" },{ Key::B,0x42,"B" },{ Key::C,0x43,"C" },{ Key::D,0x44,"D" },
        { Key::E,0x45,"E" },{ Key::F,0x46,"F" },{ Key::G,0x47,"G" },{ Key::H,0x48,"H" },
        { Key::I,0x49,"I" },{ Key::J,0x4A,"J" },{ Key::K,0x4B,"K" },{ Key::L,0x4C,"L" },
        { Key::M,0x4D,"M" },{ Key::N,0x4E,"N" },{ Key::O,0x4F,"O" },{ Key::P,0x50,"P" },
        { Key::Q,0x51,"Q" },{ Key::R,0x52,"R" },{ Key::S,0x53,"S" },{ Key::T,0x54,"T" },
        { Key::U,0x55,"U" },{ Key::V,0x56,"V" },{ Key::W,0x57,"W" },{ Key::X,0x58,"X" },
        { Key::Y,0x59,"Y" },{ Key::Z,0x5A,"Z" },
        // Digits (VK = ASCII digit)
        { Key::D0,0x30,"0" },{ Key::D1,0x31,"1" },{ Key::D2,0x32,"2" },{ Key::D3,0x33,"3" },
        { Key::D4,0x34,"4" },{ Key::D5,0x35,"5" },{ Key::D6,0x36,"6" },{ Key::D7,0x37,"7" },
        { Key::D8,0x38,"8" },{ Key::D9,0x39,"9" },
        // Function keys
        { Key::F1,VK_F1,"F1" },{ Key::F2,VK_F2,"F2" },{ Key::F3,VK_F3,"F3" },
        { Key::F4,VK_F4,"F4" },{ Key::F5,VK_F5,"F5" },{ Key::F6,VK_F6,"F6" },
        { Key::F7,VK_F7,"F7" },{ Key::F8,VK_F8,"F8" },{ Key::F9,VK_F9,"F9" },
        { Key::F10,VK_F10,"F10" },{ Key::F11,VK_F11,"F11" },{ Key::F12,VK_F12,"F12" },
        // Nav / editing
        { Key::BackSpace,VK_BACK,    "Backspace" },
        { Key::Tab,      VK_TAB,     "Tab"       },
        { Key::Return,   VK_RETURN,  "Return"    },
        { Key::Escape,   VK_ESCAPE,  "Escape"    },
        { Key::Space,    VK_SPACE,   "Space"     },
        { Key::PageUp,   VK_PRIOR,   "PageUp"    },
        { Key::PageDown, VK_NEXT,    "PageDown"  },
        { Key::End,      VK_END,     "End"       },
        { Key::Home,     VK_HOME,    "Home"      },
        { Key::Left,     VK_LEFT,    "Left"      },
        { Key::Up,       VK_UP,      "Up"        },
        { Key::Right,    VK_RIGHT,   "Right"     },
        { Key::Down,     VK_DOWN,    "Down"      },
        { Key::Insert,   VK_INSERT,  "Insert"    },
        { Key::Delete,   VK_DELETE,  "Delete"    },
        // Numpad
        { Key::Num0,VK_NUMPAD0,"Num0" },{ Key::Num1,VK_NUMPAD1,"Num1" },
        { Key::Num2,VK_NUMPAD2,"Num2" },{ Key::Num3,VK_NUMPAD3,"Num3" },
        { Key::Num4,VK_NUMPAD4,"Num4" },{ Key::Num5,VK_NUMPAD5,"Num5" },
        { Key::Num6,VK_NUMPAD6,"Num6" },{ Key::Num7,VK_NUMPAD7,"Num7" },
        { Key::Num8,VK_NUMPAD8,"Num8" },{ Key::Num9,VK_NUMPAD9,"Num9" },
        { Key::NumMul, VK_MULTIPLY, "Num*"    },
        { Key::NumAdd, VK_ADD,      "Num+"    },
        { Key::NumSub, VK_SUBTRACT, "Num-"    },
        { Key::NumDec, VK_DECIMAL,  "Num."    },
        { Key::NumDiv, VK_DIVIDE,   "Num/"    },
        { Key::NumLock,VK_NUMLOCK,  "NumLock" },
        // OEM / punctuation
        { Key::Semicolon,    VK_OEM_1,      ";"  },
        { Key::Equal,        VK_OEM_PLUS,   "="  },
        { Key::Comma,        VK_OEM_COMMA,  ","  },
        { Key::Minus,        VK_OEM_MINUS,  "-"  },
        { Key::Period,       VK_OEM_PERIOD, "."  },
        { Key::Slash,        VK_OEM_2,      "/"  },
        { Key::Grave,        VK_OEM_3,      "`"  },
        { Key::LeftBracket,  VK_OEM_4,      "["  },
        { Key::Backslash,    VK_OEM_5,      "\\" },
        { Key::RightBracket, VK_OEM_6,      "]"  },
        { Key::Quote,        VK_OEM_7,      "'"  },
        // Modifier keys (as primary key targets)
        { Key::Shift,  VK_SHIFT,    "Shift"  },
        { Key::Ctrl,   VK_CONTROL,  "Ctrl"   },
        { Key::Alt,    VK_MENU,     "Alt"    },
        { Key::LShift, VK_LSHIFT,   "LShift" },
        { Key::RShift, VK_RSHIFT,   "RShift" },
        { Key::LCtrl,  VK_LCONTROL, "LCtrl"  },
        { Key::RCtrl,  VK_RCONTROL, "RCtrl"  },
        { Key::LAlt,   VK_LMENU,    "LAlt"   },
        { Key::RAlt,   VK_RMENU,    "RAlt"   },
        // Mouse buttons. Side buttons are commonly called Mouse4/Mouse5 (= XBUTTON1/2);
        // those are the primary names so labels read "Mouse5". Mouse1-3 and the
        // MouseX1/2 / MouseLeft-Middle spellings are all accepted as aliases.
        { Key::MouseLeft,   VK_LBUTTON,  "MouseLeft"   },
        { Key::MouseRight,  VK_RBUTTON,  "MouseRight"  },
        { Key::MouseMiddle, VK_MBUTTON,  "MouseMiddle" },
        { Key::MouseX1,     VK_XBUTTON1, "Mouse4"      },
        { Key::MouseX2,     VK_XBUTTON2, "Mouse5"      },
        { Key::MouseX1,     VK_XBUTTON1, "MouseX1"     },
        { Key::MouseX2,     VK_XBUTTON2, "MouseX2"     },
        { Key::MouseLeft,   VK_LBUTTON,  "Mouse1"      },
        { Key::MouseRight,  VK_RBUTTON,  "Mouse2"      },
        { Key::MouseMiddle, VK_MBUTTON,  "Mouse3"      },
    };

    // Key enum values equal VK_* constants by design (see Lib_KeyBindings.h comment).
    // All Key↔VK conversions go through KeyToVk so any future decoupling is one change.
    static constexpr int KeyToVk(Key k) noexcept { return static_cast<int>(k); }

    // Compile-time probe: if a Key enum value drifts from its VK_* constant, the
    // corresponding assert fails with the key name as the diagnostic message.
    static_assert(KeyToVk(Key::BackSpace) == VK_BACK,       "Key::BackSpace");
    static_assert(KeyToVk(Key::Tab)       == VK_TAB,        "Key::Tab");
    static_assert(KeyToVk(Key::Return)    == VK_RETURN,     "Key::Return");
    static_assert(KeyToVk(Key::Escape)    == VK_ESCAPE,     "Key::Escape");
    static_assert(KeyToVk(Key::Space)     == VK_SPACE,      "Key::Space");
    static_assert(KeyToVk(Key::PageUp)    == VK_PRIOR,      "Key::PageUp");
    static_assert(KeyToVk(Key::PageDown)  == VK_NEXT,       "Key::PageDown");
    static_assert(KeyToVk(Key::End)       == VK_END,        "Key::End");
    static_assert(KeyToVk(Key::Home)      == VK_HOME,       "Key::Home");
    static_assert(KeyToVk(Key::Left)      == VK_LEFT,       "Key::Left");
    static_assert(KeyToVk(Key::Up)        == VK_UP,         "Key::Up");
    static_assert(KeyToVk(Key::Right)     == VK_RIGHT,      "Key::Right");
    static_assert(KeyToVk(Key::Down)      == VK_DOWN,       "Key::Down");
    static_assert(KeyToVk(Key::Insert)    == VK_INSERT,     "Key::Insert");
    static_assert(KeyToVk(Key::Delete)    == VK_DELETE,     "Key::Delete");
    static_assert(KeyToVk(Key::F1)        == VK_F1,         "Key::F1");
    static_assert(KeyToVk(Key::F12)       == VK_F12,        "Key::F12");
    static_assert(KeyToVk(Key::Num0)      == VK_NUMPAD0,    "Key::Num0");
    static_assert(KeyToVk(Key::Num9)      == VK_NUMPAD9,    "Key::Num9");
    static_assert(KeyToVk(Key::NumMul)    == VK_MULTIPLY,   "Key::NumMul");
    static_assert(KeyToVk(Key::NumDiv)    == VK_DIVIDE,     "Key::NumDiv");
    static_assert(KeyToVk(Key::Semicolon) == VK_OEM_1,      "Key::Semicolon");
    static_assert(KeyToVk(Key::Equal)     == VK_OEM_PLUS,   "Key::Equal");
    static_assert(KeyToVk(Key::Comma)     == VK_OEM_COMMA,  "Key::Comma");
    static_assert(KeyToVk(Key::Minus)     == VK_OEM_MINUS,  "Key::Minus");
    static_assert(KeyToVk(Key::Period)    == VK_OEM_PERIOD, "Key::Period");
    static_assert(KeyToVk(Key::Slash)     == VK_OEM_2,      "Key::Slash");
    static_assert(KeyToVk(Key::Grave)     == VK_OEM_3,      "Key::Grave");
    static_assert(KeyToVk(Key::Quote)     == VK_OEM_7,      "Key::Quote");
    static_assert(KeyToVk(Key::Shift)       == VK_SHIFT,      "Key::Shift");
    static_assert(KeyToVk(Key::Ctrl)       == VK_CONTROL,    "Key::Ctrl");
    static_assert(KeyToVk(Key::Alt)        == VK_MENU,       "Key::Alt");
    static_assert(KeyToVk(Key::MouseLeft)  == VK_LBUTTON,    "Key::MouseLeft");
    static_assert(KeyToVk(Key::MouseRight) == VK_RBUTTON,    "Key::MouseRight");
    static_assert(KeyToVk(Key::MouseMiddle)== VK_MBUTTON,    "Key::MouseMiddle");
    static_assert(KeyToVk(Key::MouseX1)    == VK_XBUTTON1,   "Key::MouseX1");
    static_assert(KeyToVk(Key::MouseX2)    == VK_XBUTTON2,   "Key::MouseX2");

    // ─────────────────────────────────────────────────────────────────────────
    //  Pretty-print helpers
    // ─────────────────────────────────────────────────────────────────────────

    static const char* KeyName(Key k) noexcept
    {
        for (const auto& e : kKeyTable)
            if (e.k == k) return e.name;
        return "?";
    }

    static std::string ModsName(Mod m)
    {
        std::string s;
        for (const auto& e : kModTable)
            if ((m & e.m) != Mod::None)
            {
                if (!s.empty()) s += '+';
                s += e.name;
            }
        return s;
    }

    static const char* FocusName(Focus f) noexcept
    {
        switch (f) {
        case Focus::Game:   return "Game";
        case Focus::CLI:    return "CLI";
        case Focus::Any:    return "Any";
        case Focus::Global: return "Global";
        }
        return "?";
    }

    static std::string TriggerName(Trigger t)
    {
        std::string s;
        auto add = [&](Trigger bit, const char* name) {
            if (!(t & bit)) return;
            if (!s.empty()) s += '|';
            s += name;
        };
        add(Trigger::Press,   "Press");
        add(Trigger::Release, "Release");
        add(Trigger::Held,    "Held");
        return s.empty() ? "?" : s;
    }

    static std::string BindingLabel(const BindingEntry& b)
    {
        std::string s = ModsName(b.mods);
        for (Key ck : b.opts.coKeys)
        {
            if (!s.empty()) s += '+';
            s += KeyName(ck);
        }
        if (!s.empty()) s += '+';
        s += KeyName(b.key);
        return s;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Helper: build current modifier state from physical key state
    // ─────────────────────────────────────────────────────────────────────────

    static Mod CurrentMods() noexcept
    {
        Mod m = Mod::None;
        for (const auto& e : kModTable)
            if (GetAsyncKeyState(e.vk) & 0x8000) m |= e.m;
        return m;
    }

    static bool FocusMatches(Focus f, bool game, bool cli) noexcept
    {
        switch (f)
        {
        case Focus::Global: return true;
        case Focus::Game:   return game;
        case Focus::CLI:    return cli;
        case Focus::Any:    return game || cli;
        }
        return false;
    }

    // suppress=true on a typeable key is silently downgraded when the CLI
    // currently has focus to prevent eating typed characters.
    static bool EffectiveSuppressFor(const BindingEntry& b, bool cliFocused) noexcept
    {
        if (!b.opts.suppress) return false;
        bool cliInScope = (b.opts.focus == Focus::CLI
                        || b.opts.focus == Focus::Any
                        || b.opts.focus == Focus::Global);
        if (cliInScope && cliFocused && IsTypeable(b.key)) return false;
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Shared dispatch — used by both keyboard and mouse hook procs
    // ─────────────────────────────────────────────────────────────────────────

    static bool DispatchKey(Key key, Mod mods, bool gameFocused, bool cliFocused, Trigger t)
    {
        bool suppress = false;
        std::shared_lock lock(s_mutex);
        for (const auto& b : s_bindings)
        {
            if (b.key != key)        continue;
            if (b.mods != mods)      continue;
            if (!(b.opts.trigger & t)) continue;
            if (!FocusMatches(b.opts.focus, gameFocused, cliFocused)) continue;
            bool coOk = true;
            for (Key ck : b.opts.coKeys)
                if (s_held.find(KeyToVk(ck)) == s_held.end()) { coOk = false; break; }
            if (!coOk) continue;
            if (EffectiveSuppressFor(b, cliFocused)) suppress = true;
            b.callback();
        }
        return suppress;
    }

    // Shared focus query used by both hook procs.
    static void QueryFocus(bool& gameFocused, bool& cliFocused)
    {
        HWND  fg = GetForegroundWindow();
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        gameFocused = (fgPid == GetCurrentProcessId());
        cliFocused  = (fg == reinterpret_cast<HWND>(s_cliHwnd.load(std::memory_order_relaxed)));
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  WH_KEYBOARD_LL hook proc — runs on the hook thread
    // ─────────────────────────────────────────────────────────────────────────

    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
    {
        if (nCode < 0)
            return CallNextHookEx(s_hHook, nCode, wParam, lParam);

        auto* kbs = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (kbs->flags & LLKHF_INJECTED)
            return CallNextHookEx(s_hHook, nCode, wParam, lParam);

        auto key    = static_cast<Key>(kbs->vkCode);
        auto vk     = static_cast<int>(kbs->vkCode);
        bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

        bool gameFocused, cliFocused;
        QueryFocus(gameFocused, cliFocused);
        Mod mods = CurrentMods();

        // First WM_KEYDOWN starts tracking; OS autorepeat is silently skipped.
        bool firstPress = false;
        if (isDown && s_held.find(vk) == s_held.end())
        {
            s_held[vk] = { mods, GetTickCount() };
            firstPress = true;
        }
        else if (isUp)
        {
            s_held.erase(vk);
        }

        bool suppress = false;
        if (isDown && firstPress) suppress |= DispatchKey(key, mods, gameFocused, cliFocused, Trigger::Press);
        if (isUp)                 suppress |= DispatchKey(key, mods, gameFocused, cliFocused, Trigger::Release);
        return suppress ? 1 : CallNextHookEx(s_hHook, nCode, wParam, lParam);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  WH_MOUSE_LL hook proc — runs on the hook thread
    // ─────────────────────────────────────────────────────────────────────────

    static Key MouseEventKey(WPARAM wParam, const MSLLHOOKSTRUCT* mhs) noexcept
    {
        switch (wParam)
        {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: return Key::MouseLeft;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: return Key::MouseRight;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: return Key::MouseMiddle;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            return (HIWORD(mhs->mouseData) == XBUTTON1) ? Key::MouseX1 : Key::MouseX2;
        default: return Key::MouseLeft; // unreachable
        }
    }

    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
    {
        if (nCode < 0)
            return CallNextHookEx(s_hMouseHook, nCode, wParam, lParam);

        auto* mhs = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (mhs->flags & LLMHF_INJECTED)
            return CallNextHookEx(s_hMouseHook, nCode, wParam, lParam);

        bool isDown = (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
                       wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN);
        bool isUp   = (wParam == WM_LBUTTONUP   || wParam == WM_RBUTTONUP   ||
                       wParam == WM_MBUTTONUP   || wParam == WM_XBUTTONUP);
        if (!isDown && !isUp)
            return CallNextHookEx(s_hMouseHook, nCode, wParam, lParam);

        Key key = MouseEventKey(wParam, mhs);
        int vk  = KeyToVk(key);

        bool gameFocused, cliFocused;
        QueryFocus(gameFocused, cliFocused);
        Mod mods = CurrentMods();

        bool firstPress = false;
        if (isDown && s_held.find(vk) == s_held.end())
        {
            s_held[vk] = { mods, GetTickCount() };
            firstPress = true;
        }
        else if (isUp)
        {
            s_held.erase(vk);
        }

        bool suppress = false;
        if (isDown && firstPress) suppress |= DispatchKey(key, mods, gameFocused, cliFocused, Trigger::Press);
        if (isUp)                 suppress |= DispatchKey(key, mods, gameFocused, cliFocused, Trigger::Release);
        return suppress ? 1 : CallNextHookEx(s_hMouseHook, nCode, wParam, lParam);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Held-key tick — called on the hook thread via kTickMsg
    // ─────────────────────────────────────────────────────────────────────────

    static void TickHeld()
    {
        if (s_held.empty()) return;

        HWND  fg = GetForegroundWindow();
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        bool gameFocused = (fgPid == GetCurrentProcessId());
        bool cliFocused  = (fg == reinterpret_cast<HWND>(s_cliHwnd.load(std::memory_order_relaxed)));

        DWORD now = GetTickCount();

        std::shared_lock lock(s_mutex);
        for (const auto& b : s_bindings)
        {
            if (!(b.opts.trigger & Trigger::Held)) continue;
            if (!FocusMatches(b.opts.focus, gameFocused, cliFocused)) continue;

            auto it = s_held.find(KeyToVk(b.key));
            if (it == s_held.end())        continue;
            if (it->second.mods != b.mods) continue;
            bool coOk = true;
            for (Key ck : b.opts.coKeys)
                if (s_held.find(KeyToVk(ck)) == s_held.end()) { coOk = false; break; }
            if (!coOk) continue;

            if ((now - it->second.lastFireMs) >= static_cast<DWORD>(b.opts.heldMs))
            {
                it->second.lastFireMs = now;
                b.callback();
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Hook thread — owns the message loop that drives WH_KEYBOARD_LL/MOUSE_LL
    //
    //  WM_TIMER is NOT used for the held-key tick. WH_MOUSE_LL floods the hook
    //  thread's queue with WM_MOUSEMOVE; since WM_TIMER is only synthesized when
    //  the queue is empty, it is permanently starved in any game with high mouse
    //  input. Instead a dedicated tick thread posts a custom message, which is a
    //  real queued message and fires reliably regardless of queue depth.
    // ─────────────────────────────────────────────────────────────────────────

    static constexpr UINT kTickMsg    = WM_APP + 1;
    static constexpr UINT kHeldPollMs = 50;  // tick rate; actual fire rate governed per-binding

    static std::atomic<bool> s_tickActive{ false };
    static HANDLE            s_heldTickThread = nullptr;

    static DWORD WINAPI HeldTickThreadProc(LPVOID)
    {
        while (s_tickActive.load(std::memory_order_relaxed))
        {
            Sleep(kHeldPollMs);
            if (s_tickActive.load(std::memory_order_relaxed) && s_hookThreadId)
                PostThreadMessageW(s_hookThreadId, kTickMsg, 0, 0);
        }
        return 0;
    }

    static DWORD WINAPI HookThreadProc(LPVOID)
    {
        s_hHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
        if (!s_hHook)
        {
            error("[KeyBindings] SetWindowsHookEx (keyboard) failed (err {})", GetLastError());
            return 1;
        }

        s_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, NULL, 0);
        if (!s_hMouseHook)
            warn("[KeyBindings] SetWindowsHookEx (mouse) failed (err {}) — mouse bindings disabled", GetLastError());

        s_tickActive.store(true, std::memory_order_relaxed);
        s_heldTickThread = CreateThread(NULL, 0, HeldTickThreadProc, NULL, 0, NULL);

        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0))
        {
            if (msg.message == kTickMsg) { TickHeld(); continue; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        s_tickActive.store(false, std::memory_order_relaxed);
        if (s_heldTickThread)
        {
            WaitForSingleObject(s_heldTickThread, kHeldPollMs * 3);
            CloseHandle(s_heldTickThread);
            s_heldTickThread = nullptr;
        }

        UnhookWindowsHookEx(s_hHook);
        s_hHook = nullptr;
        if (s_hMouseHook) { UnhookWindowsHookEx(s_hMouseHook); s_hMouseHook = nullptr; }
        return 0;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Public API
    // ─────────────────────────────────────────────────────────────────────────

    BindingHandle Register(Key key, Mod mods,
                           std::function<void()> callback,
                           BindingOptions opts)
    {
        std::unique_lock lock(s_mutex);

        auto sameCoKeys = [](const std::vector<Key>& a, const std::vector<Key>& b) {
            if (a.size() != b.size()) return false;
            auto sa = a; std::sort(sa.begin(), sa.end());
            auto sb = b; std::sort(sb.begin(), sb.end());
            return sa == sb;
        };

        for (const auto& b : s_bindings)
        {
            bool triggersOverlap = (static_cast<uint8_t>(b.opts.trigger)
                                  & static_cast<uint8_t>(opts.trigger)) != 0;
            if (b.key == key && b.mods == mods && b.opts.focus == opts.focus
                && sameCoKeys(b.opts.coKeys, opts.coKeys) && triggersOverlap)
            {
                BindingEntry tmp{ 0, key, mods, opts, {} };
                error("[KeyBindings] Conflict: {} [{}] {} already registered as handle {}. Second binding rejected.",
                    BindingLabel(tmp), FocusName(opts.focus), TriggerName(opts.trigger), b.handle);
                return 0;
            }
        }

        BindingHandle h = s_nextHandle++;
        s_bindings.push_back({ h, key, mods, opts, std::move(callback) });
        {
            const auto& added = s_bindings.back();
            trace("[KeyBindings] Registered {} [{}] {} handle={}",
                BindingLabel(added), FocusName(opts.focus), TriggerName(opts.trigger), h);
        }
        return h;
    }

    BindingHandle RegisterGameThread(Key key, Mod mods, std::function<void()> callback, BindingOptions opts) {
        return Register(key, mods, [callback]()
            {
                EnqueueOnce(callback);
            },
            opts
        );
    }

    void Unregister(BindingHandle handle)
    {
        if (!handle) return;
        std::unique_lock lock(s_mutex);
        auto it = std::find_if(s_bindings.begin(), s_bindings.end(),
            [handle](const BindingEntry& b) { return b.handle == handle; });
        if (it != s_bindings.end())
        {
            trace("[KeyBindings] Unregistered handle {}", handle);
            s_bindings.erase(it);
        }
    }

    void Init()
    {
        if (s_hookThread) return; // idempotent
        s_hookThread = CreateThread(NULL, 0, HookThreadProc, NULL, 0, &s_hookThreadId);
        if (!s_hookThread)
            error("[KeyBindings] CreateThread failed (err {})", GetLastError());
    }

    void Shutdown()
    {
        if (!s_hookThread) return;
        PostThreadMessageW(s_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(s_hookThread, 2000);
        CloseHandle(s_hookThread);
        s_hookThread   = nullptr;
        s_hookThreadId = 0;
    }

    void SetCLIWindow(uintptr_t hwnd)
    {
        s_cliHwnd.store(hwnd, std::memory_order_relaxed);
        trace("[KeyBindings] CLI window set to 0x{:X}", hwnd);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  CLI bind / unbind — parse a key chord and run a command on the game thread
    // ─────────────────────────────────────────────────────────────────────────

    // CLI-created bindings, tracked separately from code-registered ones so that
    // `unbind` only removes user binds and `bindings` can show the command text.
    // Touched only on the command-dispatch (worker) thread — no locking needed.
    struct CliBinding { BindingHandle handle; Key key; Mod mods; std::string command; };
    static std::vector<CliBinding> s_cliBindings;

    static bool IEquals(const std::string& a, const char* b)
    {
        size_t i = 0;
        for (; i < a.size() && b[i]; ++i)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
        return i == a.size() && b[i] == '\0';
    }

    // Parse "ctrl+shift+F2" → key + mods (modifier tokens in any order, one key).
    // Returns false on an unknown token or if no primary key was given.
    static bool ParseCombo(const std::string& spec, Key& outKey, Mod& outMods)
    {
        outMods = Mod::None;
        bool haveKey = false;
        size_t start = 0;
        for (;;)
        {
            size_t plus = spec.find('+', start);
            std::string tok = spec.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            while (!tok.empty() && std::isspace((unsigned char)tok.front())) tok.erase(tok.begin());
            while (!tok.empty() && std::isspace((unsigned char)tok.back()))  tok.pop_back();
            if (!tok.empty())
            {
                bool matched = false;
                for (const auto& m : kModTable)
                    if (IEquals(tok, m.name)) { outMods |= m.m; matched = true; break; }
                if (!matched)
                    for (const auto& e : kKeyTable)
                        if (IEquals(tok, e.name)) { outKey = e.k; haveKey = true; matched = true; break; }
                if (!matched) return false;
            }
            if (plus == std::string::npos) break;
            start = plus + 1;
        }
        return haveKey;
    }

    static std::string ComboLabel(Key key, Mod mods)
    {
        std::string s;
        for (const auto& m : kModTable) if (static_cast<bool>(mods & m.m)) { s += m.name; s += '+'; }
        for (const auto& e : kKeyTable) if (e.k == key) { s += e.name; return s; }
        s += '?';
        return s;
    }

    void RegisterCommands(CommandHandler& handler)
    {
        handler.Register("bind", [&handler](const CommandContext& ctx)
        {
            // Optional leading "-s"/"-suppress" → eat the key so the game doesn't see it.
            size_t a = 1;
            bool   suppress = false;
            if (a < ctx.args.size() && (ctx.args[a] == "-s" || ctx.args[a] == "-suppress"))
            { suppress = true; ++a; }

            if (ctx.args.size() < a + 2)
            {
                warn("[KeyBindings] usage: bind [-s] <key> <command...>  (e.g. bind F2 ramrod enhancements)");
                return;
            }
            Key key; Mod mods;
            if (!ParseCombo(ctx.args[a], key, mods))
            {
                warn("[KeyBindings] unknown key '{}' — try F1-F12, A-Z, 0-9, Num0-9, Mouse4/5, or ctrl+/shift+/alt+ combos", ctx.args[a]);
                return;
            }
            std::string cmd;
            for (size_t i = a + 1; i < ctx.args.size(); ++i) { if (i > a + 1) cmd += ' '; cmd += ctx.args[i]; }

            // Replace any existing CLI binding on the same chord.
            for (auto it = s_cliBindings.begin(); it != s_cliBindings.end(); ++it)
                if (it->key == key && it->mods == mods)
                { Unregister(it->handle); s_cliBindings.erase(it); break; }

            BindingOptions opts;
            opts.trigger  = Trigger::Press;
            opts.focus    = Focus::Game;   // fire while playing, not while typing in the CLI
            opts.suppress = suppress;      // -s → eat the key from the game
            BindingHandle h = RegisterGameThread(key, mods,
                [&handler, cmd] { handler.Dispatch(cmd); }, opts);
            if (!h)
            {
                warn("[KeyBindings] bind failed — {} already used by a non-CLI binding", ComboLabel(key, mods));
                return;
            }
            s_cliBindings.push_back({ h, key, mods, cmd });
            info("[KeyBindings] bound {}{} -> {}", ComboLabel(key, mods), suppress ? " (suppress)" : "", cmd);
        }, "keybindings", "bind [-s] <key> <command>  — run a command on key press; -s eats the key (e.g. bind F2 ramrod enhancements)");

        handler.Register("unbind", [](const CommandContext& ctx)
        {
            if (ctx.ArgCount() < 2) { warn("[KeyBindings] usage: unbind <key>"); return; }
            Key key; Mod mods;
            if (!ParseCombo(ctx.Arg(1), key, mods)) { warn("[KeyBindings] unknown key '{}'", ctx.Arg(1)); return; }
            for (auto it = s_cliBindings.begin(); it != s_cliBindings.end(); ++it)
                if (it->key == key && it->mods == mods)
                {
                    info("[KeyBindings] unbound {} (was -> {})", ComboLabel(key, mods), it->command);
                    Unregister(it->handle);
                    s_cliBindings.erase(it);
                    return;
                }
            warn("[KeyBindings] no CLI binding on {}", ComboLabel(key, mods));
        }, "keybindings", "unbind <key>  — remove a CLI key binding");

        handler.Register("bindings", [](const CommandContext& ctx)
        {
            std::shared_lock lock(s_mutex);
            if (s_bindings.empty())
            {
                info("[KeyBindings] no bindings registered");
                return;
            }
            for (const auto& b : s_bindings)
            {
                std::string line = "[" + std::to_string(b.handle) + "] "
                    + BindingLabel(b)
                    + "  " + FocusName(b.opts.focus)
                    + "  " + TriggerName(b.opts.trigger);
                if (static_cast<int>(b.opts.trigger & Trigger::Held))
                    line += " (" + std::to_string(b.opts.heldMs) + "ms)";
                if (b.opts.suppress)
                    line += "  suppress";
                for (const auto& cb : s_cliBindings)
                    if (cb.handle == b.handle) { line += "  -> " + cb.command; break; }
                info("{}", line);
            }
        }, "keybindings", "bindings  — list all registered keybindings");

        handler.Register("bindings_probe", [](const CommandContext& ctx)
        {
            int ok = 0, fail = 0;
            for (const auto& e : kKeyTable)
            {
                if (KeyToVk(e.k) == e.vk) { ++ok; continue; }
                warn("[KeyBindings] probe MISMATCH: Key '{}' enum=0x{:02X} table_vk=0x{:02X}",
                    e.name, KeyToVk(e.k), e.vk);
                ++fail;
            }
            std::string result = "[KeyBindings] probe: " + std::to_string(ok) + " keys OK";
            if (fail) result += ", " + std::to_string(fail) + " MISMATCHED";
            info("{}", result);
        }, "keybindings", "bindings_probe  — verify Key enum values match VK_* table");
    }
}
