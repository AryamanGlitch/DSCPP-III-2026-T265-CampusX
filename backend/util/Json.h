#pragma once
// Minimal JSON value + serializer (output only; requests arrive form-encoded).
//   Json::object({{"id", 1}, {"name", "Library"}}).dump()  ->  {"id":1,"name":"Library"}
#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace cx {

class Json {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), b_(b) {}
    Json(int v) : type_(Type::Int), i_(v) {}
    Json(long v) : type_(Type::Int), i_(v) {}
    Json(long long v) : type_(Type::Int), i_(v) {}
    Json(unsigned v) : type_(Type::Int), i_(static_cast<long long>(v)) {}
    Json(unsigned long v) : type_(Type::Int), i_(static_cast<long long>(v)) {}
    Json(unsigned long long v) : type_(Type::Int), i_(static_cast<long long>(v)) {}
    Json(double v) : type_(Type::Double), d_(v) {}
    Json(const char* s) : type_(Type::String), s_(s ? s : "") {}
    Json(std::string s) : type_(Type::String), s_(std::move(s)) {}

    static Json array();
    static Json object();
    static Json object(std::initializer_list<std::pair<std::string, Json>> fields);

    Json& push(Json value);                          // arrays
    Json& set(const std::string& key, Json value);   // objects (replaces existing key)
    std::size_t size() const;

    std::string dump() const;

private:
    void write(std::string& out) const;
    static void writeString(std::string& out, const std::string& s);

    Type type_ = Type::Null;
    bool b_ = false;
    long long i_ = 0;
    double d_ = 0;
    std::string s_;
    std::vector<std::string> keys_;  // object keys (parallel to items_)
    std::vector<Json> items_;        // array items or object values
};

}  // namespace cx
