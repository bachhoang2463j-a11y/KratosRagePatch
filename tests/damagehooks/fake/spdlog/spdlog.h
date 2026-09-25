#pragma once

// Test stand-in for spdlog: records what the hook logged so the harness can assert on it,
// without linking the real library. Positional "{}" placeholders are substituted for the
// two argument kinds the hook uses (const char* and bool), which is enough to tell the two
// vtable labels apart in an error message; anything else renders as "?".

#include <string>
#include <utility>
#include <vector>

namespace spdlog {

namespace test {

[[nodiscard]] inline std::vector<std::string>& messages()
{
    static std::vector<std::string> recorded;
    return recorded;
}

inline void clear()
{
    messages().clear();
}

[[nodiscard]] inline bool contains(const std::string& a_needle)
{
    for (const auto& message : messages()) {
        if (message.find(a_needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}

namespace detail {

inline void FormatArg(std::string& a_out, const char* a_value) { a_out += a_value ? a_value : "(null)"; }
inline void FormatArg(std::string& a_out, bool a_value) { a_out += a_value ? "true" : "false"; }

template <class T>
void FormatArg(std::string& a_out, const T&) { a_out += "?"; }

inline void Substitute(std::string&) {}

template <class First, class... Rest>
void Substitute(std::string& a_format, const First& a_first, const Rest&... a_rest)
{
    const auto position = a_format.find("{}");
    if (position == std::string::npos) {
        return;
    }
    std::string value;
    FormatArg(value, a_first);
    a_format.replace(position, 2, value);
    Substitute(a_format, a_rest...);
}

template <class... Args>
[[nodiscard]] std::string Render(const char* a_format, Args&&... a_args)
{
    std::string rendered{ a_format };
    Substitute(rendered, std::forward<Args>(a_args)...);
    return rendered;
}

}

template <class... Args>
void info(const char* a_format, Args&&... a_args)
{
    test::messages().emplace_back("info: " + detail::Render(a_format, std::forward<Args>(a_args)...));
}

template <class... Args>
void error(const char* a_format, Args&&... a_args)
{
    test::messages().emplace_back("error: " + detail::Render(a_format, std::forward<Args>(a_args)...));
}

}
