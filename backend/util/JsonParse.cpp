#include "JsonParse.h"

#include <cstdlib>
#include <stdexcept>

namespace cx {

const JsonValue* JsonValue::get(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& [k, v] : fields) {
        if (k == key) return &v;
    }
    return nullptr;
}

std::string JsonValue::str(const std::string& key) const {
    const JsonValue* v = get(key);
    return v && v->type == Type::String ? v->string : std::string();
}

namespace {

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    JsonValue parseDocument() {
        JsonValue v = value(0);
        skipSpace();
        if (pos_ != s_.size()) fail("unexpected text after the value");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error("Invalid JSON at " + std::to_string(pos_) + ": " + what);
    }
    void skipSpace() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) ++pos_;
    }
    bool consume(char c) {
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == c) { ++pos_; return true; }
        return false;
    }
    void expect(char c) {
        if (!consume(c)) fail(std::string("expected '") + c + "'");
    }
    void literal(const char* word) {
        for (const char* p = word; *p; ++p, ++pos_) {
            if (pos_ >= s_.size() || s_[pos_] != *p) fail(std::string("expected ") + word);
        }
    }

    JsonValue value(int depth) {
        if (depth > 64) fail("nested too deeply");
        skipSpace();
        if (pos_ >= s_.size()) fail("unexpected end");
        JsonValue v;
        char c = s_[pos_];
        if (c == '{') {
            v.type = JsonValue::Type::Object;
            ++pos_;
            if (consume('}')) return v;
            do {
                skipSpace();
                std::string key = string();
                expect(':');
                v.fields.emplace_back(std::move(key), value(depth + 1));
            } while (consume(','));
            expect('}');
        } else if (c == '[') {
            v.type = JsonValue::Type::Array;
            ++pos_;
            if (consume(']')) return v;
            do v.items.push_back(value(depth + 1));
            while (consume(','));
            expect(']');
        } else if (c == '"') {
            v.type = JsonValue::Type::String;
            v.string = string();
        } else if (c == 't') {
            literal("true");
            v.type = JsonValue::Type::Bool;
            v.boolean = true;
        } else if (c == 'f') {
            literal("false");
            v.type = JsonValue::Type::Bool;
        } else if (c == 'n') {
            literal("null");
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = s_.c_str() + pos_;
            char* end = nullptr;
            v.number = std::strtod(start, &end);
            if (end == start) fail("bad number");
            pos_ += static_cast<std::size_t>(end - start);
            v.type = JsonValue::Type::Number;
        } else {
            fail(std::string("unexpected character '") + c + "'");
        }
        return v;
    }

    unsigned hex4() {
        if (pos_ + 4 > s_.size()) fail("short \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::string string() {
        if (pos_ >= s_.size() || s_[pos_] != '"') fail("expected a string");
        ++pos_;
        std::string out;
        while (true) {
            if (pos_ >= s_.size()) fail("unterminated string");
            char c = s_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') { out += c; continue; }
            if (pos_ >= s_.size()) fail("unterminated escape");
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned low = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);  // surrogate pair -> one code point
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
    }

    const std::string& s_;
    std::size_t pos_ = 0;
};

}  // namespace

JsonValue parseJson(const std::string& text) { return Parser(text).parseDocument(); }

}  // namespace cx
