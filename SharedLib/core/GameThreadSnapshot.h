#pragma once
// GameThreadSnapshot.h — SharedLib · core (Layer 2): SDK-free.
//
// Double-buffered snapshot shared between a game-thread producer and a UI-thread
// reader. Replaces the hand-rolled "mutex + snapshot vector + request/auto/heartbeat
// atomics" that the overlay's Vars and Actors tabs each duplicated.
//
// Producer (game thread): if (s.due(now, window)) s.store(Build());
// Reader   (UI thread):   auto rows = s.read();  s.beat(now);  s.request();
//
// `due()` gates a heavy build to "explicitly requested, OR auto-refresh while the tab
// was rendered recently (a fresh beat())", so the producer skips work nobody is viewing.
// Snapshots that refresh unconditionally just call store()/read() and ignore the rest.

#include <mutex>
#include <atomic>
#include <utility>

template<typename T>
class GameThreadSnapshot
{
public:
    // Reader: copy out the current snapshot under the lock.
    T read() const { std::lock_guard lk(mutex_); return value_; }

    // Reader (no-copy): run `fn(const T&)` under the lock and return its result.
    template<typename F>
    auto with(F&& fn) const { std::lock_guard lk(mutex_); return fn(value_); }

    // Producer: publish a new snapshot.
    void store(T v)
    {
        std::lock_guard lk(mutex_);
        value_ = std::move(v);
        version_.fetch_add(1, std::memory_order_release);
    }

    // Reader: monotonically increasing counter — increments on every store().
    // Cheap atomic read; compare against a cached value to detect new data without copying.
    uint64_t version() const { return version_.load(std::memory_order_acquire); }

    // One-shot refresh request (first view / Refresh button). Consumed by due()/take().
    void request()      { requested_.store(true); }
    bool takeRequest()  { return requested_.exchange(false); }

    // Auto-refresh toggle.
    void setAuto(bool on) { auto_.store(on); }
    bool isAuto() const   { return auto_.load(); }

    // Heartbeat: reader stamps `now` each render; producer gates auto-refresh on freshness.
    void beat(uint64_t now)                       { seen_.store(now); }
    bool live(uint64_t now, uint64_t window) const { return (now - seen_.load()) < window; }

    // Producer's per-tick decision: explicit request OR (auto AND reader is live).
    // Consumes the one-shot request flag.
    bool due(uint64_t now, uint64_t window)
    {
        return takeRequest() || (auto_.load() && live(now, window));
    }

private:
    mutable std::mutex    mutex_;
    T                     value_{};
    std::atomic<bool>     requested_{ false };
    std::atomic<bool>     auto_{ false };
    std::atomic<uint64_t> seen_{ 0 };
    std::atomic<uint64_t> version_{ 0 };
};
