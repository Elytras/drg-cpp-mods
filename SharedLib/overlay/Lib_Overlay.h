#pragma once
// Lib_Overlay.h — standalone ImGui overlay (DX11, own window + own thread).
//
// Renders ImGui in OUR OWN layered, topmost, borderless window that tracks the
// game's client rect — NOT by hooking the game's swapchain. This sidesteps NVIDIA
// Streamline / DLSS entirely (the DX12 Present-hook approach crashed against
// sl.interposer's back-buffer tagging — see project memory). It is the same
// strategy UE4SS uses for its external debug GUI, extended to a transparent
// color-keyed overlay so it sits over the game instead of in a separate box.
//
// Modes:
//   Overlay — layered/topmost/borderless, color-key transparent, follows the game
//             window. Default. Toggle interactivity with Insert.
//   Window  — plain resizable opaque window (UE4SS-style), handy for 2nd monitor.
//
// Threading: everything runs on the overlay's own thread (window + D3D11 + ImGui
// render loop). Render callbacks run there — treat like the old overlay: snapshot
// game-thread state, push mutations back via EnqueueOnce(). Toggle/visibility
// setters are thread-safe (atomics); the render loop reconciles window state.

#include <atomic>
#include <functional>
#include <vector>

namespace Overlay
{
    using RenderCallback = std::function<void()>;
    using CallbackHandle = size_t;

    enum class Mode { Overlay, Window };

    // Launch the overlay thread (creates the window + device lazily on it).
    // Idempotent; returns false only if already running with a different mode.
    bool Start(Mode mode = Mode::Overlay);

    // Stop the thread and tear everything down. Safe if not running.
    void Stop();

    bool IsRunning();

    // Visibility = "interactive menu shown". Hidden => window hidden, game has
    // focus. Thread-safe; applied by the render loop on its own thread.
    void SetVisible(bool v);
    bool ToggleVisible();   // returns the resulting visible state
    bool IsVisible();

    // Cap the overlay's render-loop frame rate (default 200). Clamped [1,1000].
    void SetTargetFps(int fps);
    int  GetTargetFps();

    // Overlay toggle key (a Win32 VK code; default VK_END). This is the single
    // source of truth: Start() registers a global KeyBindings press on it to show
    // the overlay, and the overlay window's WndProc closes on it — so changing it
    // updates both. Setting it re-registers the binding immediately. Thread-safe.
    void     SetToggleKey(uint16_t vk);
    uint16_t GetToggleKey();

    // Key-capture for a "press a key" rebind widget. BeginKeyCapture() makes the overlay
    // grab the next real key / mouse button (Esc cancels); poll TakeCapturedKey() each
    // frame — it returns true once with the captured VK (which is a Key, since Key==VK).
    void BeginKeyCapture();
    void CancelKeyCapture();
    bool IsCapturingKey();
    bool TakeCapturedKey(uint16_t* outVk, uint8_t* outMods = nullptr);

    // Convenience: register a self-contained ImGui window. `draw` is called
    // between ImGui::Begin(name)/End() each frame while the overlay is visible —
    // the easy way to add a panel without touching the render loop. `windowFlags`
    // are ImGuiWindowFlags (int to keep this header ImGui-free).
    CallbackHandle AddPanel(const char* name, RenderCallback draw, int windowFlags = 0);

    // Draw callbacks — each runs every frame while visible (atomic shared_ptr
    // swap, lock-free dispatch).
    CallbackHandle AddRenderCallback(RenderCallback cb);
    bool           RemoveRenderCallback(CallbackHandle h);
    void           ClearRenderCallbacks();

} // namespace Overlay
