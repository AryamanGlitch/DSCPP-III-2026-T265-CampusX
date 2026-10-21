#include "util/Json.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace cx {

Json Json::array() { Json j; j.type_ = Type::Array; return j; }
Json Json::object() { Json j; j.type_ = Type::Object; return j; }

Json Json::object(std::initializer_list<std::pair<std::string, Json>> fields) {
    Json j = object();
    for (const auto& [k, v] : fields) j.set(k, v);
    return j;
}

Json& Json::push(Json value) {
    if (type_ != Type::Array) throw std::logic_error("Json::push on non-array");
    items_.push_back(std::move(value));
    return *this;
}

Json& Json::set(const std::string& key, Json value) {
    if (type_ != Type::Object) throw std::logic_error("Json::set on non-object");
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) { items_[i] = std::move(value); return *this; }
    }
    keys_.push_back(key);
    items_.push_back(std::move(value));
    return *this;
}

std::size_t Json::size() const { return items_.size(); }

std::string Json::dump() const {
    std::string out;
    write(out);
    return out;
}

void Json::writeString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);  // UTF-8 bytes pass through unchanged
                }
        }
    }
    out += '"';
}

void Json::write(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b_ ? "true" : "false"; break;
        case Type::Int: out += std::to_string(i_); break;
        case Type::Double: {
            if (!std::isfinite(d_)) { out += "null"; break; }  // JSON has no Infinity/NaN
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.10g", d_);
            out += buf;
            break;
        }
        case Type::String: writeString(out, s_); break;
        case Type::Array:
            out += '[';
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i) out += ',';
                items_[i].write(out);
            }
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i) out += ',';
                writeString(out, keys_[i]);
                out += ':';
                items_[i].write(out);
            }
            out += '}';
            break;
    }
}

}  // namespace cx
