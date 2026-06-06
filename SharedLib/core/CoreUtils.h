#pragma once
// CoreUtils.h — SharedLib · core (Layer 2): game-agnostic helpers, NO SDK.
//
// See CAPABILITIES.md and REFACTOR_PLAN.md. This is the shared home for utilities
// that used to live (duplicated) in each game's Lib_Utils.h — so SharedLib code can
// use them instead of reinventing (the reason the overlay reimplemented strtof).
// Header-only by design: no per-game .cpp, nothing to add to a .vcxproj.
//
// Globals (not namespaced) to match the existing call sites (e.g. `SafeStof(s)`).

#include <string>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <thread>
#include <concepts>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <algorithm>

// ── Safe numeric parsers (no-throw; return 0 on empty/invalid) ──────────────────
inline int64_t  SafeStoll (const std::string& s) noexcept { if (s.empty()) return 0;   try { return std::stoll(s);  } catch (...) { return 0; } }
inline uint64_t SafeStoull(const std::string& s) noexcept { if (s.empty()) return 0;   try { return std::stoull(s); } catch (...) { return 0; } }
inline float    SafeStof  (const std::string& s) noexcept { if (s.empty()) return 0.f; try { return std::stof(s);  } catch (...) { return 0.f; } }
inline double   SafeStod  (const std::string& s) noexcept { if (s.empty()) return 0.0; try { return std::stod(s);  } catch (...) { return 0.0; } }

// ── Variadic membership helpers ─────────────────────────────────────────────────
template<typename T, typename... Ts> bool NoneOf(T c, Ts... ts) { return ((c != ts) && ...); }
template<typename T, typename... Ts> bool AnyOf (T c, Ts... ts) { return ((c == ts) || ...); }

// ── Timing / threading ──────────────────────────────────────────────────────────
inline void     SleepNow (uint64_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds{ ms }); }
inline uint64_t GetTimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── Float comparison (relative epsilon) ─────────────────────────────────────────
inline bool NearlyEqual(double a, double b, double epsilon = 1e-9)
{
    return std::fabs(a - b) <= epsilon * (std::max)({ 1.0, std::fabs(a), std::fabs(b) });
}

// ── Indexable-container access ──────────────────────────────────────────────────
template<typename T>
concept IsIndexableContainer = requires (const T& c, size_t i) {
    typename T::value_type;
    { c.size()  } -> std::same_as<size_t>;
    { c.empty() } -> std::same_as<bool>;
    { c[i]      } -> std::convertible_to<typename T::value_type>;
};

template<IsIndexableContainer Container>
typename Container::value_type GetOrDefault(
    const Container& c,
    const typename Container::value_type& def,
    std::optional<size_t> index = std::nullopt)
{
    if (!c.empty())
    {
        if (index && *index < c.size()) return c[*index];
        return c[0];
    }
    return def;
}

// =========================================================================
// Numeric ranges
//
//   numeric_range<T>(begin, end)         — step ±1 by direction
//   numeric_range<T>(begin, end, step)   — explicit step
//   range(begin, end)  /  range(begin, end, step)  /  range(end)
// =========================================================================

namespace nr_detail {

    template<typename T>
    concept NumericPrimitive =
        (std::integral<T>
            && !std::same_as<T, bool>
            && !std::same_as<T, wchar_t>
            && !std::same_as<T, char8_t>
            && !std::same_as<T, char16_t>
            && !std::same_as<T, char32_t>)
        || std::floating_point<T>
#if defined(__FLT16_MAX__)
        || std::same_as<T, _Float16>
#endif
        ;

    template<NumericPrimitive T>
    constexpr T default_step() noexcept { return static_cast<T>(1); }

} // namespace nr_detail


template<nr_detail::NumericPrimitive T>
class numeric_range_iterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type        = T;
    using difference_type   = std::ptrdiff_t;
    using pointer           = const T*;
    using reference         = T;

    constexpr numeric_range_iterator(T cur, T end, T step) noexcept
        : cur_(cur), end_(end), step_(step) {}

    constexpr T       operator*()  const noexcept { return cur_; }
    constexpr pointer operator->() const noexcept { return &cur_; }

    constexpr numeric_range_iterator& operator++() noexcept { cur_ = static_cast<T>(cur_ + step_); return *this; }
    constexpr numeric_range_iterator  operator++(int) noexcept { auto c = *this; ++(*this); return c; }

    constexpr bool operator==(const numeric_range_iterator& o) const noexcept {
        return step_ > T{0} ? cur_ >= o.cur_ : cur_ <= o.cur_;
    }
    constexpr bool operator!=(const numeric_range_iterator& o) const noexcept { return !(*this == o); }

private:
    T cur_, end_, step_;
};


template<nr_detail::NumericPrimitive T>
class numeric_range {
public:
    using iterator = numeric_range_iterator<T>;

    constexpr numeric_range(T begin, T end)
        : begin_(begin), end_(end)
        , step_(end >= begin ? nr_detail::default_step<T>() : static_cast<T>(-1)) {}

    constexpr numeric_range(T begin, T end, T step)
        : begin_(begin), end_(end), step_(step)
    {
        if (step == T{0}) throw std::invalid_argument("numeric_range: step must not be zero");
    }

    constexpr iterator begin() const noexcept {
        if (step_ > T{0} && begin_ >= end_) return sentinel();
        if (step_ < T{0} && begin_ <= end_) return sentinel();
        return iterator{ begin_, end_, step_ };
    }
    constexpr iterator end()   const noexcept { return sentinel(); }

    constexpr bool empty() const noexcept {
        if (step_ > T{0}) return begin_ >= end_;
        if (step_ < T{0}) return begin_ <= end_;
        return true;
    }

    constexpr std::ptrdiff_t size() const noexcept requires std::integral<T> {
        if (step_ > T{0} && end_ > begin_)
            return static_cast<std::ptrdiff_t>((end_ - begin_ + step_ - T{1}) / step_);
        if (step_ < T{0} && begin_ > end_)
            return static_cast<std::ptrdiff_t>((begin_ - end_ - step_ - T{1}) / (-step_));
        return 0;
    }

private:
    constexpr iterator sentinel() const noexcept { return iterator{ end_, end_, step_ }; }
    T begin_, end_, step_;
};

template<nr_detail::NumericPrimitive T> numeric_range(T, T)    -> numeric_range<T>;
template<nr_detail::NumericPrimitive T> numeric_range(T, T, T) -> numeric_range<T>;

template<nr_detail::NumericPrimitive T> constexpr auto range(T begin, T end)        { return numeric_range<T>(begin, end); }
template<nr_detail::NumericPrimitive T> constexpr auto range(T begin, T end, T step){ return numeric_range<T>(begin, end, step); }
template<nr_detail::NumericPrimitive T> constexpr auto range(T end)                 { return numeric_range<T>(T{0}, end); }
