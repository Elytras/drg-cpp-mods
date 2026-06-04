#pragma once
#ifndef EXTRA_LEAN
#define EXTRA_LEAN
#endif

#include <Windows.h>
#include <vector>
#include <string>
#include <string_view>
#include <concepts>
#include <type_traits>
#include <unordered_set>

namespace StringLib
{

// Helper alias to make the function signature readable
template <typename T>
using StringElem = typename T::value_type::value_type;

template <typename T>
using StringTraits = typename T::value_type::traits_type;

template<class Container>
    requires std::same_as<
        typename Container::value_type,
            std::basic_string<StringElem<Container>, StringTraits<Container>>
    >
Container SplitString(
    // We derive CharT and Traits directly from the Container
    std::basic_string_view<StringElem<Container>, StringTraits<Container>> input,
    std::basic_string_view<StringElem<Container>, StringTraits<Container>> delimiter
)
{
    Container result{};

    if (delimiter.empty()) {
        result.emplace_back(input);
        return result;
    }

    std::size_t pos = 0;

    while (true) {
        auto next = input.find(delimiter, pos);
        if (next == std::decay_t<decltype(input)>::npos) {
            result.emplace_back(input.substr(pos));
            break;
        }

        result.emplace_back(input.substr(pos, next - pos));
        pos = next + delimiter.size();
    }

    return result;
}


    template<class Container, class CharT, class Traits = std::char_traits<CharT>>
        requires std::same_as<typename Container::value_type, std::basic_string_view<CharT, Traits>>
    std::basic_string<CharT, Traits> JoinStringArray(const Container& array, std::basic_string_view<CharT, Traits> delimiter) {
        using string_type = std::basic_string<CharT, Traits>;

        if (array.empty()) {
            return {};
        }

        // Compute total size
        std::size_t total_size = 0;
        std::size_t count = 0;

        for (const auto& sv : array) {
            total_size += sv.size();
            ++count;
        }

        total_size += delimiter.size() * (count - 1);

        string_type result;
        result.reserve(total_size);

        auto it = array.begin();
        result.append(it->data(), it->size());
        ++it;

        for (; it != array.end(); ++it) {
            result.append(delimiter.data(), delimiter.size());
            result.append(it->data(), it->size());
        }

        return result;
    }


    struct CaseInsensitiveHash
    {
        size_t operator()(const std::string& s) const
        {
            std::string lower;
            lower.reserve(s.size());

            for (unsigned char c : s)
                lower += std::tolower(c);

            return std::hash<std::string>{}(lower);
        }
    };

    struct CaseInsensitiveEqual
    {
        bool operator()(const std::string& a, const std::string& b) const
        {
            if (a.size() != b.size())
                return false;

            for (size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                    return false;
            }
            return true;
        }
    };

    template <typename CharT, typename Traits = std::char_traits<CharT>>
    std::basic_string_view<CharT, Traits> Trim(std::basic_string_view<CharT, Traits> s) {
        const auto first = s.find_first_not_of(" \t\n\r\f\v");
        if (first == std::string_view::npos) return {};
        const auto last = s.find_last_not_of(" \t\n\r\f\v");
        return s.substr(first, (last - first + 1));
    }

    template <typename CharT, typename Traits = std::char_traits<CharT>>
    std::basic_string<CharT, Traits> ToLower(std::basic_string_view<CharT, Traits> s) {
        std::basic_string<CharT, Traits> result;
        result.reserve(s.size());
        for (auto c : s)
            result += static_cast<CharT>(std::tolower(static_cast<unsigned char>(c)));
        return result;
    }

    template <typename CharT, typename Traits = std::char_traits<CharT>>
    std::basic_string<CharT, Traits> ToUpper(std::basic_string_view<CharT, Traits> s) {
        std::basic_string<CharT, Traits> result;
        result.reserve(s.size());
        for (auto c : s)
            result += static_cast<CharT>(std::toupper(static_cast<unsigned char>(c)));
        return result;
    }

    template <typename CharT, typename Traits = std::char_traits<CharT>>
    std::basic_string<CharT, Traits> Replace(
        std::basic_string_view<CharT, Traits> input,
        std::basic_string_view<CharT, Traits> from,
        std::basic_string_view<CharT, Traits> to)
    {
        std::basic_string<CharT, Traits> result;
        result.reserve(input.size()); // Heuristic
        size_t pos = 0;
        size_t find_pos;

        while ((find_pos = input.find(from, pos)) != std::string_view::npos) {
            result.append(input.substr(pos, find_pos - pos));
            result.append(to);
            pos = find_pos + from.size();
        }
        result.append(input.substr(pos));
        return result;
    }

    template <typename CharT>
    bool EqualsIgnoreCase(std::basic_string_view<CharT> a, std::basic_string_view<CharT> b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](auto char1, auto char2) {
            return std::tolower(static_cast<unsigned char>(char1)) ==
                std::tolower(static_cast<unsigned char>(char2));
            });
    }

    template <typename CharT>
    bool Contains(std::basic_string_view<CharT> haystack, std::basic_string_view<CharT> needle) {
        return haystack.find(needle) != std::basic_string_view<CharT>::npos;
    }

    template <typename CharT>
    bool StartsWith(std::basic_string_view<CharT> haystack, std::basic_string_view<CharT> needle) {
        return haystack.size() >= needle.size() &&
            haystack.compare(0, needle.size(), needle) == 0;
    }

    template <typename CharT>
    bool EndsWith(std::basic_string_view<CharT> haystack, std::basic_string_view<CharT> needle) {
        return haystack.size() >= needle.size() &&
            haystack.compare(haystack.size() - needle.size(), needle.size(), needle) == 0;
    }

    // Returns true if `haystack` starts with any of the supplied needles.
    template <typename CharT, typename Needles>
    bool StartsWithAnyOf(std::basic_string_view<CharT> haystack, const Needles& needles) {
        for (const auto& needle : needles) {
            if (StartsWith(haystack, std::basic_string_view<CharT>(needle)))
                return true;
        }
        return false;
    }

    // Returns true if `haystack` contains any of the supplied needles.
    template <typename CharT, typename Needles>
    bool ContainsAnyOf(std::basic_string_view<CharT> haystack, const Needles& needles) {
        for (const auto& needle : needles) {
            if (Contains(haystack, std::basic_string_view<CharT>(needle)))
                return true;
        }
        return false;
    }

    // Returns true if `haystack` ends with any of the supplied needles.
    template <typename CharT, typename Needles>
    bool EndsWithAnyOf(std::basic_string_view<CharT> haystack, const Needles& needles) {
        for (const auto& needle : needles) {
            if (EndsWith(haystack, std::basic_string_view<CharT>(needle)))
                return true;
        }
        return false;
    }

    inline std::wstring ToWide(std::string_view s) {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring result(len, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), len);
        return result;
    }

    inline std::string ToNarrow(std::wstring_view s) {
        if (s.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string result(len, 0);
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), len, nullptr, nullptr);
        return result;
    }

    using CaseInsensitiveSet = std::unordered_set<
        std::string,
        CaseInsensitiveHash,
        CaseInsensitiveEqual
    >;


}
