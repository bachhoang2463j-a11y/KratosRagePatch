#pragma once
// ---------------------------------------------------------------------------
// KratosRagePatch 离线单元测试支撑（自研极简框架，无第三方依赖、无网络）。
//
// 约定：
//  * 本项目中 C++ 字符串字面量一律保持 ASCII，避免 MSVC 默认源码字符集下乱码；
//    中文只出现在注释与 tests/fixtures/*.json 数据里（JSON 由本文件按 UTF-8 解析）。
//  * 用例通过 KR_TEST(名字) 注册，TestMain.cpp 顺序执行；
//    断言失败只记录不中断，main 汇总后以非零码退出。
//  * 名字以 spec_gap_ 开头的 fixture 用例是"按契约编写、预期暴露 src 缺陷"的用例，
//    运行器会显式标注。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace krtest {

// =============================== 极简 JSON =================================
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type{Type::Null};
    bool boolean{false};
    double number{0.0};
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    bool IsNull() const { return type == Type::Null; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }

    const Json* Find(const std::string& key) const
    {
        if (type != Type::Object) return nullptr;
        for (const auto& f : fields) {
            if (f.first == key) return &f.second;
        }
        return nullptr;
    }
    bool Has(const std::string& key) const { return Find(key) != nullptr; }

    int AsInt(int fallback = 0) const
    {
        return type == Type::Number ? static_cast<int>(number) : fallback;
    }
    std::uint32_t AsU32(std::uint32_t fallback = 0) const
    {
        if (type != Type::Number) return fallback;
        return static_cast<std::uint32_t>(number < 0.0 ? 0.0 : number);
    }
    double AsNumber(double fallback = 0.0) const
    {
        return type == Type::Number ? number : fallback;
    }
    bool AsBool(bool fallback = false) const
    {
        return type == Type::Bool ? boolean : fallback;
    }
    std::string AsString(std::string fallback = std::string()) const
    {
        return type == Type::String ? text : fallback;
    }
};

namespace detail {

inline void SkipWs(const std::string& s, std::size_t& i)
{
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
        } else {
            break;
        }
    }
}

inline void AppendUtf8(std::uint32_t cp, std::string& out)
{
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool Run(Json& out)
    {
        SkipWs(s_, i_);
        if (!ParseValue(out)) return false;
        SkipWs(s_, i_);
        if (i_ != s_.size()) return Fail("trailing characters");
        return true;
    }

    const std::string& Error() const { return error_; }

private:
    bool Fail(const std::string& message)
    {
        error_ = message + " at offset " + std::to_string(i_);
        return false;
    }

    bool ParseValue(Json& out)
    {
        SkipWs(s_, i_);
        if (i_ >= s_.size()) return Fail("unexpected end of input");
        const char c = s_[i_];
        if (c == '{') return ParseObject(out);
        if (c == '[') return ParseArray(out);
        if (c == '"') {
            out.type = Json::Type::String;
            return ParseString(out.text);
        }
        if (c == 't') return ParseLiteral("true", out, true);
        if (c == 'f') return ParseLiteral("false", out, false);
        if (c == 'n') {
            if (s_.compare(i_, 4, "null") != 0) return Fail("bad literal");
            i_ += 4;
            out.type = Json::Type::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
        return Fail("unexpected character");
    }

    bool ParseLiteral(const char* literal, Json& out, bool value)
    {
        const std::size_t len = std::strlen(literal);
        if (s_.compare(i_, len, literal) != 0) return Fail("bad literal");
        i_ += len;
        out.type = Json::Type::Bool;
        out.boolean = value;
        return true;
    }

    bool ParseArray(Json& out)
    {
        out.type = Json::Type::Array;
        ++i_;  // '['
        SkipWs(s_, i_);
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        while (true) {
            Json item;
            if (!ParseValue(item)) return false;
            out.items.push_back(item);
            SkipWs(s_, i_);
            if (i_ >= s_.size()) return Fail("unterminated array");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseObject(Json& out)
    {
        out.type = Json::Type::Object;
        ++i_;  // '{'
        SkipWs(s_, i_);
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        while (true) {
            SkipWs(s_, i_);
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs(s_, i_);
            if (i_ >= s_.size() || s_[i_] != ':') return Fail("expected ':'");
            ++i_;
            Json value;
            if (!ParseValue(value)) return false;
            out.fields.push_back(std::make_pair(key, value));
            SkipWs(s_, i_);
            if (i_ >= s_.size()) return Fail("unterminated object");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseString(std::string& out)
    {
        if (i_ >= s_.size() || s_[i_] != '"') return Fail("expected string");
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '"') {
                ++i_;
                return true;
            }
            if (c == '\\') {
                ++i_;
                if (i_ >= s_.size()) return Fail("bad escape");
                const char e = s_[i_++];
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!ParseHex4(cp)) return false;
                    AppendUtf8(cp, out);
                    break;
                }
                default: return Fail("unknown escape");
                }
                continue;
            }
            out.push_back(c);
            ++i_;
        }
        return Fail("unterminated string");
    }

    bool ParseHex4(std::uint32_t& out)
    {
        if (i_ + 4 > s_.size()) return Fail("bad \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return Fail("bad hex digit");
            }
            out = (out << 4) | digit;
        }
        return true;
    }

    bool ParseNumber(Json& out)
    {
        const std::size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++i_;
            } else {
                break;
            }
        }
        const std::string token = s_.substr(start, i_ - start);
        if (token.empty()) return Fail("bad number");
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size()) return Fail("bad number");
        out.type = Json::Type::Number;
        out.number = value;
        return true;
    }

    const std::string& s_;
    std::size_t i_{0};
    std::string error_;
};

}  // namespace detail

inline bool ParseJson(const std::string& text, Json& out, std::string& error)
{
    detail::Parser parser(text);
    if (!parser.Run(out)) {
        error = parser.Error();
        return false;
    }
    return true;
}

inline bool LoadJsonFile(const std::string& path, Json& out, std::string& error)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return ParseJson(buffer.str(), out, error);
}

inline bool FileExists(const std::string& path)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    return file.good();
}

inline std::string DirOfFile(const std::string& file)
{
    const std::size_t slash = file.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : file.substr(0, slash);
}

// 依次尝试：显式参数、常见相对路径、测试源文件自身所在目录。
inline std::string ResolveFixtureDir(const std::string& explicitDir, const char* sourceFile)
{
    std::vector<std::string> candidates;
    if (!explicitDir.empty()) candidates.push_back(explicitDir);
    candidates.push_back("tests/fixtures");
    candidates.push_back("../tests/fixtures");
    candidates.push_back("../../tests/fixtures");
    const std::string fromSource = DirOfFile(sourceFile) + "/fixtures";
    candidates.push_back(fromSource);

    for (const std::string& candidate : candidates) {
        if (FileExists(candidate + "/hit_cases.json")) return candidate;
    }
    return fromSource;
}

// =============================== 断言框架 ==================================
struct Failure {
    std::string where;
    std::string message;
};

inline std::vector<Failure>& Failures()
{
    static std::vector<Failure> value;
    return value;
}

inline std::size_t& Checks()
{
    static std::size_t value = 0;
    return value;
}

inline std::string& CurrentCase()
{
    static std::string value;
    return value;
}

inline void SetCurrentCase(const std::string& name) { CurrentCase() = name; }

inline void RecordFailure(const char* file, int line, const std::string& message)
{
    Failure failure;
    failure.where = std::string(file) + ":" + std::to_string(line);
    if (!CurrentCase().empty()) failure.where = CurrentCase() + " @ " + failure.where;
    failure.message = message;
    Failures().push_back(failure);
}

inline void ExpectTrue(bool value, const std::string& what, const char* file, int line)
{
    ++Checks();
    if (!value) RecordFailure(file, line, "expected true: " + what);
}

inline void ExpectFalse(bool value, const std::string& what, const char* file, int line)
{
    ++Checks();
    if (value) RecordFailure(file, line, "expected false: " + what);
}

inline void ExpectEqI(long long actual, long long expected, const std::string& what, const char* file, int line)
{
    ++Checks();
    if (actual != expected) {
        RecordFailure(file, line, what + ": expected " + std::to_string(expected) +
                                      ", actual " + std::to_string(actual));
    }
}

inline void ExpectEqB(bool actual, bool expected, const std::string& what, const char* file, int line)
{
    ++Checks();
    if (actual != expected) {
        RecordFailure(file, line, what + ": expected " + (expected ? "true" : "false") +
                                      ", actual " + (actual ? "true" : "false"));
    }
}

inline void ExpectEqStr(const std::string& actual, const std::string& expected,
                        const std::string& what, const char* file, int line)
{
    ++Checks();
    if (actual != expected) {
        RecordFailure(file, line, what + ": expected \"" + expected + "\", actual \"" + actual + "\"");
    }
}

// ------------------------------- 用例注册 -----------------------------------
struct TestCase {
    std::string name;
    void (*fn)();
};

inline std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> value;
    return value;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry().push_back(TestCase{name, fn}); }
};

#define KR_TEST(name)                                                       \
    static void name();                                                     \
    static ::krtest::Registrar krt_registrar_##name(#name, &name);           \
    static void name()

#define KRT_EXPECT(cond)          ::krtest::ExpectTrue(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define KRT_EXPECT_FALSE(cond)    ::krtest::ExpectFalse(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define KRT_EQI(actual, expected) ::krtest::ExpectEqI(static_cast<long long>(actual), static_cast<long long>(expected), #actual, __FILE__, __LINE__)
#define KRT_EQB(actual, expected) ::krtest::ExpectEqB(static_cast<bool>(actual), static_cast<bool>(expected), #actual, __FILE__, __LINE__)
#define KRT_EQSTR(actual, expected) ::krtest::ExpectEqStr((actual), (expected), #actual, __FILE__, __LINE__)
#define KRT_EXPECT_HAS(opt)       ::krtest::ExpectTrue((opt).has_value(), #opt " has value", __FILE__, __LINE__)
#define KRT_EXPECT_NONE(opt)      ::krtest::ExpectFalse((opt).has_value(), #opt " has no value", __FILE__, __LINE__)

// --------------------------- fixture 驱动入口 -------------------------------
// 由 RageStateTests.cpp / ActivationTests.cpp 实现；返回失败用例数。
int RunHitFixtureCases(const std::string& fixtureDir);
int RunActivationFixtureCases(const std::string& fixtureDir);

}  // namespace krtest
