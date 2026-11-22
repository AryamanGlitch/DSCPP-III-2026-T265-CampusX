#pragma once
// Minimal JSON reader (recursive descent), the counterpart of Json.h which only writes.
// Used to read Gemini API responses. Throws std::runtime_error on invalid input.
#include <string>
#include <utility>
#include <vector>

namespace cx {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<JsonValue> items;                            // arrays
    std::vector<std::pair<std::string, JsonValue>> fields;   // objects, in source order

    const JsonValue* get(const std::string& key) const;  // nullptr if missing or not an object
    std::string str(const std::string& key) const;      // "" if missing or not a string
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }
};

JsonValue parseJson(const std::string& text);

}  // namespace cx
