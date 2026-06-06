// Lib_Overlay.cpp — standalone DX11 ImGui overlay (own window + own thread).
//
// Why standalone: hooking the game's DX12 swapchain crashed against NVIDIA
// Streamline's back-buffer tagging (see project memory). UE4SS sidesteps the same
// problem by rendering its GUI in a separate window with its own device — we do
// the same, but as a layered/color-keyed topmost window tracking the game's
// client rect so it reads as an in-game overlay.
//
// INTEGRATION:
//   Overlay::Start(Overlay::Mode::Overlay);          // from OnModsLoaded
//   Overlay::AddRenderCallback([]{ ImGui::ShowDemoWindow(); });
//   // bind Insert -> Overlay::ToggleVisible() via Lib_KeyBindings
//   Overlay::Stop();                                 // from OnModsUnloading

#include "Lib_Overlay.h"
#include "Common.h"
#include "Lib_Forward.h"   // info/warn/error
#include "Lib_KeyBindings.h"

#include <thread>
#include <mutex>
#include <memory>
#include <algorithm>
#include <string>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>   // IDXGIFactory2::CreateSwapChainForComposition
#include <dcomp.h>     // DirectComposition (GPU-composited transparent overlay)
#include <chrono>
#include <timeapi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace Overlay
{
    namespace
    {
        // ── Callback list (lock-free dispatch via atomic shared_ptr) ────────────
        struct Entry { CallbackHandle handle; RenderCallback fn; };
        using CallbackList = std::vector<Entry>;
        std::atomic<std::shared_ptr<const CallbackList>> g_callbacks{};
        std::mutex      g_cbMutex;
        CallbackHandle  g_nextHandle{ 1 };

        // ── Thread + lifecycle ──────────────────────────────────────────────────
        std::thread       g_thread;
        std::atomic<bool> g_running{ false };
        std::atomic<bool> g_stop{ false };
        std::atomic<bool> g_desiredVisible{ false };
        std::atomic<Mode> g_mode{ Mode::Overlay };
        std::atomic<int>  g_targetFps{ 60 };

        // Overlay toggle key (VK). Default End. Owns the global show-binding and the
        // WndProc close key — single source of truth so a rebind updates both.
        std::atomic<uint16_t> g_toggleKey{ (uint16_t)Key::End };
        BindingHandle         s_toggleBind = 0;

        // (Re)register the global KeyBindings press that shows the overlay. Focus::Game
        // (fires while the game has focus → opens); closing while the overlay is
        // focused is handled by the WndProc. Idempotent.
        void RegisterToggleBind()
        {
            if (s_toggleBind) { KeyBindings::Unregister(s_toggleBind); s_toggleBind = 0; }
            BindingOptions o;
            o.trigger  = Trigger::Press;
            o.focus    = Focus::Game;
            o.suppress = true;
            o.label    = "Toggle overlay";
            s_toggleBind = KeyBindings::Register((Key)g_toggleKey.load(), Mod::None,
                                                 [] { ToggleVisible(); }, o);
        }

        // ── Window + D3D11 (touched only on the overlay thread) ─────────────────
        HWND                    s_hwnd = nullptr;
        HWND                    s_gameHwnd = nullptr;
        ATOM                    s_wndClass = 0;
        ImGuiContext* s_imgui = nullptr;
        ID3D11Device* s_device = nullptr;
        ID3D11DeviceContext* s_ctx = nullptr;
        IDXGISwapChain1* s_swap = nullptr;
        ID3D11RenderTargetView* s_rtv = nullptr;
        // DirectComposition (transparent, GPU-composited — replaces WS_EX_LAYERED).
        IDCompositionDevice*    s_dcompDevice = nullptr;
        IDCompositionTarget*    s_dcompTarget = nullptr;
        IDCompositionVisual*    s_dcompVisual = nullptr;
        bool                    s_appliedVisible = false;
        UINT                    s_width = 0, s_height = 0;

        constexpr wchar_t kClassName[] = L"ElytrasOverlayWnd";

        // ── Find the game's main window (this process, top-level, visible) ──────
        struct FindCtx { DWORD pid; HWND best; LONG bestArea; HWND exclude; };
        BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
        {
            auto* c = reinterpret_cast<FindCtx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != c->pid || hwnd == c->exclude) return TRUE;
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
            RECT r{};
            if (!GetClientRect(hwnd, &r)) return TRUE;
            LONG area = (r.right - r.left) * (r.bottom - r.top);
            if (area > c->bestArea) { c->bestArea = area; c->best = hwnd; }
            return TRUE;
        }
        HWND FindGameWindow()
        {
            FindCtx c{ GetCurrentProcessId(), nullptr, 0, s_hwnd };
            EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&c));
            return c.best;
        }

        // ── D3D11 device / RT (UE4SS pattern) ───────────────────────────────────
        void CreateRT()
        {
            ID3D11Texture2D* back = nullptr;
            if (SUCCEEDED(s_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back)
            {
                s_device->CreateRenderTargetView(back, nullptr, &s_rtv);
                back->Release();
            }
        }
        void CleanupRT() { if (s_rtv) { s_rtv->Release(); s_rtv = nullptr; } }

        // Composition swapchains need a non-zero size up front (unlike hwnd
        // swapchains). s_width/s_height are set by CreateOverlayWindow first.
        bool CreateDeviceD3D(HWND hwnd)
        {
            const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // required for DComp
            D3D_FEATURE_LEVEL fl;
            const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                    want, _countof(want), D3D11_SDK_VERSION, &s_device, &fl, &s_ctx)))
                return false;

            IDXGIDevice*  dxgiDevice = nullptr;
            IDXGIAdapter* adapter    = nullptr;
            IDXGIFactory2* factory   = nullptr;
            if (FAILED(s_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;
            dxgiDevice->GetAdapter(&adapter);
            adapter->GetParent(IID_PPV_ARGS(&factory));

            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width            = s_width  ? s_width  : 1280;
            sd.Height           = s_height ? s_height : 720;
            sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount      = 2;
            sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            sd.AlphaMode        = DXGI_ALPHA_MODE_PREMULTIPLIED;   // ImGui-over-cleared-zero = premultiplied

            HRESULT hr = factory->CreateSwapChainForComposition(s_device, &sd, nullptr, &s_swap);
            if (SUCCEEDED(hr))
            {
                hr = DCompositionCreateDevice(dxgiDevice, IID_PPV_ARGS(&s_dcompDevice));
                if (SUCCEEDED(hr))
                {
                    s_dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &s_dcompTarget);
                    s_dcompDevice->CreateVisual(&s_dcompVisual);
                    s_dcompVisual->SetContent(s_swap);
                    s_dcompTarget->SetRoot(s_dcompVisual);
                    s_dcompDevice->Commit();
                }
            }

            if (factory)    factory->Release();
            if (adapter)    adapter->Release();
            if (dxgiDevice) dxgiDevice->Release();
            if (FAILED(hr)) return false;

            CreateRT();
            return true;
        }

        void CleanupDeviceD3D()
        {
            CleanupRT();
            if (s_dcompVisual) { s_dcompVisual->Release(); s_dcompVisual = nullptr; }
            if (s_dcompTarget) { s_dcompTarget->Release(); s_dcompTarget = nullptr; }
            if (s_dcompDevice) { s_dcompDevice->Release(); s_dcompDevice = nullptr; }
            if (s_swap)   { s_swap->Release();   s_swap = nullptr; }
            if (s_ctx)    { s_ctx->Release();    s_ctx = nullptr; }
            if (s_device) { s_device->Release(); s_device = nullptr; }
        }

        // ── WndProc ─────────────────────────────────────────────────────────────
        LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
        {
            if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l)) return TRUE;
            switch (msg)
            {
            case WM_SIZE:
                if (s_device && w != SIZE_MINIMIZED)
                {
                    CleanupRT();
                    s_swap->ResizeBuffers(0, (UINT)LOWORD(l), (UINT)HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
                    s_width = LOWORD(l); s_height = HIWORD(l);
                    CreateRT();
                }
                return 0;
            case WM_KEYDOWN:
                // Close on the toggle key (whatever it's set to) or Escape.
                if (w == g_toggleKey.load() || w == (uint64)Key::Escape) { g_desiredVisible.store(false); return 0; }
                break;
            case WM_SYSCOMMAND:
                if ((w & 0xfff0) == SC_KEYMENU) return 0;   // no ALT menu
                break;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, w, l);
        }

        // ── Window creation ─────────────────────────────────────────────────────
        bool CreateOverlayWindow(Mode mode)
        {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
            wc.lpszClassName = kClassName;
            s_wndClass = RegisterClassExW(&wc);

            int x = 100, y = 100, cx = 1280, cy = 720;
            DWORD style   = WS_OVERLAPPEDWINDOW;
            DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP;   // required for DComp content to show

            if (mode == Mode::Overlay)
            {
                s_gameHwnd = FindGameWindow();
                if (s_gameHwnd)
                {
                    RECT cr{}; GetClientRect(s_gameHwnd, &cr);
                    POINT tl{ cr.left, cr.top }; ClientToScreen(s_gameHwnd, &tl);
                    x = tl.x; y = tl.y; cx = cr.right - cr.left; cy = cr.bottom - cr.top;
                }
                style    = WS_POPUP;
                exStyle |= WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
            }

            s_width = (UINT)cx; s_height = (UINT)cy;   // composition swapchain needs the size

            s_hwnd = CreateWindowExW(exStyle, kClassName, L"Elytras Overlay", style,
                x, y, cx, cy, nullptr, nullptr, wc.hInstance, nullptr);
            if (!s_hwnd) return false;

            return CreateDeviceD3D(s_hwnd);
        }

        // ── Track the game window's client rect (Overlay mode) ──────────────────
        void FollowGameWindow()
        {
            if (g_mode.load() != Mode::Overlay) return;
            if (!s_gameHwnd || !IsWindow(s_gameHwnd)) {
                s_gameHwnd = FindGameWindow();
                if (!s_gameHwnd) return;
            }

            if (IsIconic(s_gameHwnd)) return;

            RECT cr{}; GetClientRect(s_gameHwnd, &cr);
            POINT tl{ cr.left, cr.top }; ClientToScreen(s_gameHwnd, &tl);
            int cx = cr.right - cr.left, cy = cr.bottom - cr.top;

            if (cx <= 0 || cy <= 0) return;

            // Cache window parameters to avoid redundant SetWindowPos calls
            static int lastX = 0, lastY = 0, lastCX = 0, lastCY = 0;
            static HWND lastForeground = nullptr;

            HWND currentForeground = GetForegroundWindow();
            bool sizeOrPosChanged = (tl.x != lastX || tl.y != lastY || cx != lastCX || cy != lastCY);
            bool focusChanged = (currentForeground != lastForeground);

            if (!sizeOrPosChanged && !focusChanged) {
                return;
            }

            lastX = tl.x; lastY = tl.y; lastCX = cx; lastCY = cy;
            lastForeground = currentForeground;

            UINT flags = SWP_NOACTIVATE | SWP_NOREDRAW;

            if (currentForeground == s_gameHwnd || currentForeground == s_hwnd) {
                SetWindowPos(s_hwnd, HWND_TOPMOST, tl.x, tl.y, cx, cy, flags);
            }
            else {
                SetWindowPos(s_hwnd, HWND_NOTOPMOST, tl.x, tl.y, cx, cy, flags);
            }
        }

        // Reliable foreground steal. Plain SetForegroundWindow fails across threads
        // due to Windows' foreground lock; attaching to the current foreground
        // thread's input queue first lifts that restriction.
        void ForceForeground(HWND hwnd)
        {
            HWND  fg    = GetForegroundWindow();
            DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
            DWORD myTid = GetCurrentThreadId();
            if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, TRUE);
            BringWindowToTop(hwnd);
            SetForegroundWindow(hwnd);
            SetActiveWindow(hwnd);
            SetFocus(hwnd);
            if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, FALSE);
        }

        // ── Apply show/hide + focus on visibility transitions ───────────────────
        void ApplyVisibility(bool visible)
        {
            if (visible == s_appliedVisible) return;
            s_appliedVisible = visible;
            if (visible)
            {
                ShowWindow(s_hwnd, SW_SHOWNA);
                ForceForeground(s_hwnd);
            }
            else
            {
                ShowWindow(s_hwnd, SW_HIDE);
                if (s_gameHwnd && IsWindow(s_gameHwnd)) ForceForeground(s_gameHwnd);
            }
        }

        // While the overlay is open, reclaim input focus whenever the GAME window
        // regains the foreground (e.g. after alt-tabbing back) — otherwise the game
        // keeps eating input until the user clicks the overlay.
        void ReclaimFocusIfGameForeground()
        {
            static HWND s_lastFg = nullptr;
            HWND fg = GetForegroundWindow();
            if (fg == s_gameHwnd && s_lastFg != s_gameHwnd)
                ForceForeground(s_hwnd);
            s_lastFg = fg;
        }

        // ── The render thread ───────────────────────────────────────────────────
        // ── The render thread ───────────────────────────────────────────────────
        void ThreadMain(Mode mode)
        {
            if (!CreateOverlayWindow(mode))
            {
                error("[Overlay] failed to create window/device");
                CleanupDeviceD3D();
                g_running.store(false);
                return;
            }

            // Set Windows scheduler resolution to 1ms for high-precision sleep
            timeBeginPeriod(1);

            IMGUI_CHECKVERSION();
            s_imgui = ImGui::CreateContext();
            ImGui::SetCurrentContext(s_imgui);
            // Persist window layout (position/size/collapsed) to a stable path under
            // %LOCALAPPDATA% so it survives restarts and hot-reloads. ImGui loads it on
            // the first frame and auto-saves on change. Static so the pointer outlives
            // this scope (ImGui keeps the raw pointer).
            static std::string s_iniPath;
            {
                const char* base = std::getenv("LOCALAPPDATA");
                std::string dir = (base ? std::string(base) : std::string(".")) + "\\ElytrasOverlay";
                CreateDirectoryA(dir.c_str(), nullptr);
                s_iniPath = dir + "\\overlay_imgui.ini";
            }
            ImGui::GetIO().IniFilename = s_iniPath.c_str();
            // Keyboard nav off on purpose: it makes ImGui consume Tab to move focus
            // between widgets/windows, which steals Tab from the console's
            // command-completion callback. The overlay is mouse-driven.
            ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
            ImGui::StyleColorsDark();
            ImGui_ImplWin32_Init(s_hwnd);
            ImGui_ImplDX11_Init(s_device, s_ctx);

            info("[Overlay] running ({} mode)", mode == Mode::Overlay ? "overlay" : "window");

            // Fully transparent clear (premultiplied alpha) — DComp shows the game through it.
            const float clearTransparent[4] = { 0.f, 0.f, 0.f, 0.f };

            ULONGLONG lastWindowSync = 0;

            while (!g_stop.load())
            {
                // Start timing the frame
                auto frameStart = std::chrono::high_resolution_clock::now();

                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                    if (msg.message == WM_QUIT) g_stop.store(true);
                }
                if (g_stop.load()) break;

                const bool visible = g_desiredVisible.load();
                ApplyVisibility(visible);

                if (!visible) {
                    Sleep(33); // When hidden, drop thread activity to ~30 FPS
                    continue;
                }

                // Reclaim input if the game grabbed foreground (e.g. alt-tab back).
                ReclaimFocusIfGameForeground();

                // Call FollowGameWindow once every 3 frames (~50ms) instead of every frame
                ULONGLONG now = GetTickCount64();
                if (now - lastWindowSync > 50) {
                    FollowGameWindow();
                    lastWindowSync = now;
                }

                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();

                if (auto cbs = g_callbacks.load())
                    for (const auto& e : *cbs)
                    {
                        try { if (e.fn) e.fn(); }
                        catch (const std::exception&) {}
                    }

                ImGui::Render();
                s_ctx->OMSetRenderTargets(1, &s_rtv, nullptr);
                s_ctx->ClearRenderTargetView(s_rtv, clearTransparent);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

                // Present immediately (0, 0) since we are managing the frame rate ourselves
                s_swap->Present(0, 0);

                // Calculate frame duration and sleep to maintain the target FPS
                const double targetFrameTimeMs = 1000.0 / std::clamp(g_targetFps.load(), 1, 1000);
                auto frameEnd = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> elapsed = frameEnd - frameStart;

                if (elapsed.count() < targetFrameTimeMs)
                {
                    DWORD sleepTime = static_cast<DWORD>(targetFrameTimeMs - elapsed.count());
                    if (sleepTime > 0)
                    {
                        Sleep(sleepTime);
                    }
                }
            }

            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(s_imgui);
            s_imgui = nullptr;
            CleanupDeviceD3D();
            if (s_hwnd) { DestroyWindow(s_hwnd); s_hwnd = nullptr; }
            if (s_wndClass) { UnregisterClassW(kClassName, GetModuleHandleW(nullptr)); s_wndClass = 0; }
            s_appliedVisible = false;
            g_running.store(false);

            timeEndPeriod(1);

            info("[Overlay] stopped");
        }
    } // anonymous namespace

    // ─────────────────────────────────────────────────────────────────────────
    // Public API
    // ─────────────────────────────────────────────────────────────────────────
    bool Start(Mode mode)
    {
        if (g_running.load()) return g_mode.load() == mode;
        g_mode.store(mode);
        g_stop.store(false);
        g_running.store(true);
        RegisterToggleBind();                 // global show-key (default End)
        g_thread = std::thread(ThreadMain, mode);
        return true;
    }

    void Stop()
    {
        if (!g_running.load() && !g_thread.joinable()) return;
        g_stop.store(true);
        if (s_hwnd) PostMessageW(s_hwnd, WM_NULL, 0, 0);   // wake the message loop
        if (g_thread.joinable()) g_thread.join();
        if (s_toggleBind) { KeyBindings::Unregister(s_toggleBind); s_toggleBind = 0; }
    }

    bool IsRunning() { return g_running.load(); }

    void SetVisible(bool v) { g_desiredVisible.store(v); }
    bool ToggleVisible() { bool n = !g_desiredVisible.load(); g_desiredVisible.store(n); return n; }
    bool IsVisible() { return g_desiredVisible.load(); }

    void SetToggleKey(uint16_t vk)
    {
        if (!vk) return;
        g_toggleKey.store(vk);
        if (g_running.load()) RegisterToggleBind();   // re-register the global show-key
    }
    uint16_t GetToggleKey() { return g_toggleKey.load(); }

    void SetTargetFps(int fps) { g_targetFps.store(std::clamp(fps, 1, 1000)); }
    int  GetTargetFps()        { return g_targetFps.load(); }

    CallbackHandle AddPanel(const char* name, RenderCallback draw, int windowFlags)
    {
        if (!name || !draw) return 0;
        std::string title = name;
        return AddRenderCallback([title, draw, windowFlags]
        {
            if (ImGui::Begin(title.c_str(), nullptr, (ImGuiWindowFlags)windowFlags)) draw();
            ImGui::End();
        });
    }

    CallbackHandle AddRenderCallback(RenderCallback cb)
    {
        if (!cb) return 0;
        std::lock_guard lock(g_cbMutex);
        auto old = g_callbacks.load();
        auto next = old ? std::make_shared<CallbackList>(*old) : std::make_shared<CallbackList>();
        CallbackHandle h = g_nextHandle++;
        next->push_back({ h, std::move(cb) });
        g_callbacks.store(std::move(next));
        return h;
    }

    bool RemoveRenderCallback(CallbackHandle h)
    {
        std::lock_guard lock(g_cbMutex);
        auto old = g_callbacks.load();
        if (!old) return false;
        auto next = std::make_shared<CallbackList>(*old);
        auto it = std::remove_if(next->begin(), next->end(), [h](const Entry& e) { return e.handle == h; });
        if (it == next->end()) return false;
        next->erase(it, next->end());
        g_callbacks.store(std::move(next));
        return true;
    }

    void ClearRenderCallbacks()
    {
        std::lock_guard lock(g_cbMutex);
        g_callbacks.store(std::make_shared<CallbackList>());
    }

} // namespace Overlay