// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// A small JSON reader for extension manifests, filter rules and messages.
// Strings are UTF-8. Each value remembers where it came from in the source
// text, so callers can copy a value's raw JSON without re-serializing it.
namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool asBool(bool fallback = false) const { return isBool() ? m_bool : fallback; }
    double asNumber(double fallback = 0) const { return isNumber() ? m_number : fallback; }
    const std::string& asString() const { return m_string; } // empty if not a string

    // Arrays
    size_t size() const { return isArray() ? m_array.size() : isObject() ? m_object.size() : 0; }
    const Value& operator[](size_t index) const;
    const std::vector<Value>& items() const { return m_array; }

    // Objects (keys keep their original order)
    const Value& operator[](const std::string& key) const; // null Value if missing
    const Value* find(const std::string& key) const;
    bool has(const std::string& key) const { return find(key) != nullptr; }
    const std::vector<std::pair<std::string, Value>>& members() const { return m_object; }

    // Position of this value in the parsed text, for copying its raw JSON.
    size_t start() const { return m_start; }
    size_t end() const { return m_end; }

private:
    friend class Parser;
    Type m_type { Type::Null };
    bool m_bool { false };
    double m_number { 0 };
    std::string m_string;
    std::vector<Value> m_array;
    std::vector<std::pair<std::string, Value>> m_object;
    size_t m_start { 0 };
    size_t m_end { 0 };
};

// Parses `text`. Allows a UTF-8 byte order mark and // or /* */ comments
// (Chrome accepts both in manifest.json). On error returns false and sets
// `error`.
bool parse(const std::string& text, Value& out, std::string* error = nullptr);

// Writes a value back out as compact JSON (comments dropped, strings
// re-escaped), safe to embed in JavaScript source.
std::string serialize(const Value&);

// Escapes a UTF-8 string as a JSON string literal (with quotes).
std::string quote(const std::string& text);

} // namespace json
