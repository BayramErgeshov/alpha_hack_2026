#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace json {

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Value() : value_(nullptr) {}
    Value(std::nullptr_t) : value_(nullptr) {}
    Value(bool value) : value_(value) {}
    Value(int value) : value_(static_cast<double>(value)) {}
    Value(long long value) : value_(static_cast<double>(value)) {}
    Value(double value) : value_(std::isfinite(value) ? value : 0.0) {}
    Value(const char* value) : value_(std::string(value ? value : "")) {}
    Value(std::string value) : value_(std::move(value)) {}
    Value(Array value) : value_(std::move(value)) {}
    Value(Object value) : value_(std::move(value)) {}

    static Value object() { return Object{}; }
    static Value array() { return Array{}; }
    bool is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
    bool is_bool() const { return std::holds_alternative<bool>(value_); }
    bool is_number() const { return std::holds_alternative<double>(value_); }
    bool is_string() const { return std::holds_alternative<std::string>(value_); }
    bool is_array() const { return std::holds_alternative<Array>(value_); }
    bool is_object() const { return std::holds_alternative<Object>(value_); }

    bool as_bool(bool fallback = false) const {
        return is_bool() ? std::get<bool>(value_) : fallback;
    }
    double as_number(double fallback = 0.0) const {
        return is_number() ? std::get<double>(value_) : fallback;
    }
    const std::string& as_string() const {
        static const std::string empty;
        return is_string() ? std::get<std::string>(value_) : empty;
    }
    const Array& as_array() const {
        static const Array empty;
        return is_array() ? std::get<Array>(value_) : empty;
    }
    Array& as_array() {
        if (!is_array()) value_ = Array{};
        return std::get<Array>(value_);
    }
    const Object& as_object() const {
        static const Object empty;
        return is_object() ? std::get<Object>(value_) : empty;
    }

    const Value& get(const std::string& key) const {
        static const Value null_value;
        if (!is_object()) return null_value;
        const auto& object = std::get<Object>(value_);
        auto it = object.find(key);
        return it == object.end() ? null_value : it->second;
    }
    Value& operator[](const std::string& key) {
        if (!is_object()) value_ = Object{};
        return std::get<Object>(value_)[key];
    }
    void push_back(Value value) { as_array().push_back(std::move(value)); }

    std::string dump() const {
        std::string out;
        append_dump(out);
        return out;
    }

    static Value parse(const std::string& input) {
        struct Parser {
            const std::string& s;
            size_t pos = 0;
            size_t depth = 0;

            void ws() {
                while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) ++pos;
            }
            [[noreturn]] void fail(const char* message) const {
                throw std::runtime_error(std::string("JSON ") + message + " at byte " + std::to_string(pos));
            }
            bool take(char c) {
                ws();
                if (pos < s.size() && s[pos] == c) { ++pos; return true; }
                return false;
            }
            static void utf8(std::string& out, std::uint32_t cp) {
                if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
                else if (cp <= 0x7FF) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else if (cp <= 0xFFFF) {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            }
            std::uint32_t hex4() {
                if (pos + 4 > s.size()) fail("has incomplete unicode escape");
                std::uint32_t cp = 0;
                for (int i = 0; i < 4; ++i) {
                    const char c = s[pos++];
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= c - '0';
                    else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
                    else fail("has invalid unicode escape");
                }
                return cp;
            }
            std::string string_value() {
                if (!take('"')) fail("expected string");
                std::string out;
                while (pos < s.size()) {
                    unsigned char c = static_cast<unsigned char>(s[pos++]);
                    if (c == '"') return out;
                    if (c < 0x20) fail("contains a control character in a string");
                    if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
                    if (pos >= s.size()) fail("has an incomplete escape");
                    switch (s[pos++]) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        std::uint32_t cp = hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (pos + 2 > s.size() || s[pos] != '\\' || s[pos + 1] != 'u') fail("has an invalid surrogate pair");
                            pos += 2;
                            std::uint32_t low = hex4();
                            if (low < 0xDC00 || low > 0xDFFF) fail("has an invalid surrogate pair");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) fail("has an unexpected low surrogate");
                        utf8(out, cp);
                        break;
                    }
                    default: fail("has an invalid escape");
                    }
                }
                fail("has an unterminated string");
            }
            Value value() {
                ws();
                if (pos >= s.size()) fail("is incomplete");
                if (s[pos] == '"') return string_value();
                if (s[pos] == '{') {
                    if (++depth > 64) fail("is nested too deeply");
                    ++pos;
                    Object object;
                    ws();
                    if (take('}')) { --depth; return object; }
                    do {
                        ws();
                        if (pos >= s.size() || s[pos] != '"') fail("object key must be a string");
                        std::string key = string_value();
                        if (!take(':')) fail("expected ':'");
                        object[std::move(key)] = value();
                    } while (take(','));
                    if (!take('}')) fail("expected '}'");
                    --depth;
                    return object;
                }
                if (s[pos] == '[') {
                    if (++depth > 64) fail("is nested too deeply");
                    ++pos;
                    Array array;
                    ws();
                    if (take(']')) { --depth; return array; }
                    do { array.push_back(value()); } while (take(','));
                    if (!take(']')) fail("expected ']'");
                    --depth;
                    return array;
                }
                if (s.compare(pos, 4, "true") == 0) { pos += 4; return true; }
                if (s.compare(pos, 5, "false") == 0) { pos += 5; return false; }
                if (s.compare(pos, 4, "null") == 0) { pos += 4; return nullptr; }
                const size_t begin = pos;
                if (s[pos] == '-') ++pos;
                if (pos >= s.size()) fail("has an invalid number");
                if (s[pos] == '0') ++pos;
                else {
                    if (s[pos] < '1' || s[pos] > '9') fail("contains an invalid value");
                    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
                }
                if (pos < s.size() && s[pos] == '.') {
                    ++pos;
                    const size_t digits = pos;
                    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
                    if (digits == pos) fail("has an invalid fraction");
                }
                if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                    ++pos;
                    if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
                    const size_t digits = pos;
                    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
                    if (digits == pos) fail("has an invalid exponent");
                }
                double number = 0;
                try { number = std::stod(s.substr(begin, pos - begin)); }
                catch (...) { fail("has an invalid number"); }
                return number;
            }
        } parser{ input };
        Value result = parser.value();
        parser.ws();
        if (parser.pos != input.size()) throw std::runtime_error("JSON has trailing data");
        return result;
    }

private:
    Storage value_;

    static void quote(std::string& out, const std::string& text) {
        static const char* hex = "0123456789abcdef";
        out.push_back('"');
        for (unsigned char c : text) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
                else out.push_back(static_cast<char>(c));
            }
        }
        out.push_back('"');
    }
    void append_dump(std::string& out) const {
        if (is_null()) { out += "null"; return; }
        if (is_bool()) { out += as_bool() ? "true" : "false"; return; }
        if (is_number()) {
            std::ostringstream stream;
            stream.precision(15);
            stream << as_number();
            out += stream.str();
            return;
        }
        if (is_string()) { quote(out, as_string()); return; }
        if (is_array()) {
            out.push_back('[');
            bool first = true;
            for (const auto& item : as_array()) {
                if (!first) out.push_back(',');
                first = false;
                item.append_dump(out);
            }
            out.push_back(']');
            return;
        }
        out.push_back('{');
        bool first = true;
        for (const auto& item : as_object()) {
            if (!first) out.push_back(',');
            first = false;
            quote(out, item.first);
            out.push_back(':');
            item.second.append_dump(out);
        }
        out.push_back('}');
    }
};

} // namespace json
