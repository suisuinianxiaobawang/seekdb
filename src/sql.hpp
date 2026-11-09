#pragma once
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mini {
struct Error : std::runtime_error {
    std::string code;
    Error(std::string c, const std::string& message)
        : std::runtime_error(message), code(std::move(c)) {}
};

[[noreturn]] inline void syntax() { throw Error("SYNTAX_ERROR", "Invalid SQL syntax"); }
[[noreturn]] inline void invalid(const std::string& message) {
    throw Error("INVALID_VALUE", message);
}

inline bool letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
inline std::string lower(std::string_view s) {
    std::string result(s);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}

enum class Kind { End, Word, Identifier, String, Number, Symbol };
struct Token {
    Kind kind = Kind::End;
    std::string_view text;
};

class Parser {
    std::string_view input;
    Token token;

    Token next() {
        while (true) {
            while (!input.empty() && space(input.front())) input.remove_prefix(1);
            if (input.starts_with("--") || input.starts_with("#")) {
                auto end = input.find('\n');
                input.remove_prefix(end == input.npos ? input.size() : end);
            } else if (input.starts_with("/*")) {
                auto end = input.find("*/", 2);
                if (end == input.npos) syntax();
                input.remove_prefix(end + 2);
            } else break;
        }
        if (input.empty()) return {};
        char c = input.front();
        size_t n = 1;
        Kind kind = Kind::Symbol;
        if (letter(c)) {
            kind = Kind::Word;
            while (n < input.size() && (letter(input[n]) || digit(input[n]))) ++n;
        } else if (c == '\'' || c == '"' || c == '`') {
            kind = c == '`' ? Kind::Identifier : Kind::String;
            for (; n < input.size(); ++n) {
                if (input[n] == c) {
                    if (n + 1 < input.size() && input[n + 1] == c) ++n;
                    else break;
                } else if (input[n] == '\\' && c != '`' && n + 1 < input.size()) ++n;
            }
            if (n == input.size()) syntax();
            ++n;
        } else if (digit(c) || c == '+' || c == '-' || c == '.') {
            kind = Kind::Number;
            n = (c == '+' || c == '-') ? 1 : 0;
            while (n < input.size() && digit(input[n])) ++n;
            if (n < input.size() && input[n] == '.') {
                ++n;
                while (n < input.size() && digit(input[n])) ++n;
            }
            if (n < input.size() && (input[n] == 'e' || input[n] == 'E')) {
                ++n;
                if (n < input.size() && (input[n] == '+' || input[n] == '-')) ++n;
                while (n < input.size() && digit(input[n])) ++n;
            }
        } else if ((c == '<' || c == '>' || c == '!') && input.size() > 1 &&
                   (input[1] == '=' || (c == '<' && input[1] == '>'))) {
            n = 2;
        }
        auto result = input.substr(0, n);
        input.remove_prefix(n);
        return {kind, result};
    }

    static std::string unquote(std::string_view s) {
        std::string out;
        out.reserve(s.size() - 2);
        for (size_t i = 1; i + 1 < s.size(); ++i) {
            char c = s[i];
            if (c == s.front()) ++i;
            else if (c == '\\' && s.front() != '`') {
                c = s[++i];
                switch (c) {
                    case '0': c = '\0'; break;
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'b': c = '\b'; break;
                    case 'Z': c = '\x1a'; break;
                }
            }
            out.push_back(c);
        }
        return out;
    }

public:
    explicit Parser(std::string_view sql) : input(sql), token(next()) {}
    Token take() {
        Token current = token;
        token = next();
        return current;
    }
    Kind kind() const { return token.kind; }
    bool is(std::string_view value) const {
        if (token.kind == Kind::Symbol) return token.text == value;
        if (token.kind != Kind::Word || token.text.size() != value.size()) return false;
        for (size_t i = 0; i < value.size(); ++i) {
            char c = token.text[i];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != value[i]) return false;
        }
        return true;
    }
    bool accept(std::string_view value) {
        if (!is(value)) return false;
        take();
        return true;
    }
    void need(std::string_view value) { if (!accept(value)) syntax(); }
    void finish() {
        accept(";");
        if (kind() != Kind::End) syntax();
    }
    std::string identifier() {
        if (kind() == Kind::Word) return lower(take().text);
        if (kind() == Kind::Identifier) {
            auto name = lower(unquote(take().text));
            if (!name.empty()) return name;
        }
        syntax();
    }
    std::string string() {
        if (kind() != Kind::String) invalid("Expected string");
        return unquote(take().text);
    }
    int64_t integer() {
        if (kind() != Kind::Number) invalid("Expected BIGINT");
        auto s = take().text;
        if (s.starts_with('+')) s.remove_prefix(1);
        int64_t value = 0;
        auto result = std::from_chars(s.data(), s.data() + s.size(), value);
        if (result.ec != std::errc{} || result.ptr != s.data() + s.size()) invalid("Invalid BIGINT");
        return value;
    }
    uint32_t positive_size() {
        auto n = integer();
        if (n <= 0 || n > std::numeric_limits<uint32_t>::max()) invalid("Invalid column size");
        return static_cast<uint32_t>(n);
    }
    std::vector<float> vector(uint32_t dimension) {
        if (kind() == Kind::String) {
            auto literal = string();
            Parser nested(literal);
            if (!nested.is("[")) invalid("Expected vector literal");
            auto result = nested.vector(dimension);
            nested.finish();
            return result;
        }
        if (!accept("[")) invalid("Expected vector");
        std::vector<float> result;
        if (!accept("]")) {
            do {
                if (kind() != Kind::Number || result.size() >= dimension) invalid("Invalid vector");
                auto text = take().text;
                if (text.starts_with('+')) text.remove_prefix(1);
                float value;
#ifndef __APPLE__
                auto converted = std::from_chars(text.data(), text.data() + text.size(), value);
                if (converted.ec == std::errc{} && converted.ptr == text.data() + text.size()) {
                    if (!std::isfinite(value)) invalid("Invalid float32");
                    result.push_back(value);
                    continue;
                }
#endif
                char small[64];
                std::string large;
                const char* number;
                if (text.size() < sizeof small) {
                    std::memcpy(small, text.data(), text.size());
                    small[text.size()] = '\0';
                    number = small;
                } else {
                    large.assign(text);
                    number = large.c_str();
                }
                char* end;
                value = std::strtof(number, &end);
                if (end == number || end != number + text.size() || !std::isfinite(value))
                    invalid("Invalid float32");
                result.push_back(value);
            } while (accept(","));
            need("]");
        }
        if (result.size() != dimension) invalid("Wrong vector dimension");
        return result;
    }
};

inline size_t unicode_length(std::string_view text) {
    size_t characters = 0;
    for (size_t i = 0; i < text.size(); ++characters) {
        unsigned char lead = text[i++];
        if (lead < 0x80) continue;
        unsigned n;
        uint32_t cp;
        if (lead >= 0xc2 && lead <= 0xdf) { n = 1; cp = lead & 31; }
        else if (lead >= 0xe0 && lead <= 0xef) { n = 2; cp = lead & 15; }
        else if (lead >= 0xf0 && lead <= 0xf4) { n = 3; cp = lead & 7; }
        else invalid("Invalid UTF-8");
        if (text.size() - i < n) invalid("Invalid UTF-8");
        for (unsigned j = 0; j < n; ++j) {
            unsigned char byte = text[i++];
            if ((byte & 0xc0) != 0x80) invalid("Invalid UTF-8");
            cp = (cp << 6) | (byte & 63);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) invalid("Invalid UTF-8");
    }
    return characters;
}
} // namespace mini
