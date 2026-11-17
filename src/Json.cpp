// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

static const Value& nullValue()
{
    static const Value value;
    return value;
}

const Value& Value::operator[](size_t index) const
{
    return isArray() && index < m_array.size() ? m_array[index] : nullValue();
}

const Value* Value::find(const std::string& key) const
{
    if (!isObject())
        return nullptr;
    for (auto& member : m_object) {
        if (member.first == key)
            return &member.second;
    }
    return nullptr;
}

const Value& Value::operator[](const std::string& key) const
{
    const Value* value = find(key);
    return value ? *value : nullValue();
}

class Parser {
public:
    Parser(const std::string& text)
        : m_text(text)
    {
    }

    bool run(Value& out, std::string* error)
    {
        // Skip a UTF-8 byte order mark.
        if (m_text.size() >= 3 && m_text.compare(0, 3, "\xEF\xBB\xBF") == 0)
            m_pos = 3;
        bool ok = parseValue(out, 0) && (skipSpace(), m_pos == m_text.size() || fail("unexpected text after the value"));
        if (!ok && error)
            *error = m_error + " (at byte " + std::to_string(m_pos) + ")";
        return ok;
    }

private:
    bool fail(const char* message)
    {
        if (m_error.empty())
            m_error = message;
        return false;
    }

    void skipSpace()
    {
        while (m_pos < m_text.size()) {
            char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_pos;
            } else if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/') {
                while (m_pos < m_text.size() && m_text[m_pos] != '\n')
                    ++m_pos;
            } else if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '*') {
                size_t close = m_text.find("*/", m_pos + 2);
                m_pos = close == std::string::npos ? m_text.size() : close + 2;
            } else
                break;
        }
    }

    bool parseValue(Value& out, int depth)
    {
        if (depth > 200)
            return fail("nested too deeply");
        skipSpace();
        if (m_pos >= m_text.size())
            return fail("unexpected end of text");
        out.m_start = m_pos;
        char c = m_text[m_pos];
        bool ok;
        switch (c) {
        case '{': ok = parseObject(out, depth); break;
        case '[': ok = parseArray(out, depth); break;
        case '"': out.m_type = Value::Type::String; ok = parseString(out.m_string); break;
        case 't': ok = parseLiteral("true"); out.m_type = Value::Type::Bool; out.m_bool = true; break;
        case 'f': ok = parseLiteral("false"); out.m_type = Value::Type::Bool; out.m_bool = false; break;
        case 'n': ok = parseLiteral("null"); out.m_type = Value::Type::Null; break;
        default: ok = parseNumber(out); break;
        }
        out.m_end = m_pos;
        return ok;
    }

    bool parseLiteral(const char* word)
    {
        size_t length = strlen(word);
        if (m_text.compare(m_pos, length, word) != 0)
            return fail("invalid value");
        m_pos += length;
        return true;
    }

    bool parseNumber(Value& out)
    {
        size_t begin = m_pos;
        if (m_pos < m_text.size() && (m_text[m_pos] == '-' || m_text[m_pos] == '+'))
            ++m_pos;
        while (m_pos < m_text.size() && strchr("0123456789.eE+-", m_text[m_pos]))
            ++m_pos;
        if (begin == m_pos)
            return fail("invalid value");
        std::string number = m_text.substr(begin, m_pos - begin);
        char* end = nullptr;
        out.m_number = strtod(number.c_str(), &end);
        if (!end || *end)
            return fail("invalid number");
        out.m_type = Value::Type::Number;
        return true;
    }

    static void appendUTF8(std::string& out, unsigned code)
    {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool readHex4(unsigned& code)
    {
        if (m_pos + 4 > m_text.size())
            return fail("invalid \\u escape");
        code = 0;
        for (int i = 0; i < 4; ++i) {
            char c = m_text[m_pos++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= c - '0';
            else if (c >= 'a' && c <= 'f') code |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') code |= c - 'A' + 10;
            else return fail("invalid \\u escape");
        }
        return true;
    }

    bool parseString(std::string& out)
    {
        ++m_pos; // opening quote
        out.clear();
        while (m_pos < m_text.size()) {
            // Copy runs of plain characters at once (fast for big filter lists).
            size_t run = m_pos;
            while (run < m_text.size() && m_text[run] != '"' && m_text[run] != '\\')
                ++run;
            out.append(m_text, m_pos, run - m_pos);
            m_pos = run;
            if (m_pos >= m_text.size())
                break;
            char c = m_text[m_pos++];
            if (c == '"')
                return true;
            // Backslash escape.
            if (m_pos >= m_text.size())
                break;
            char e = m_text[m_pos++];
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
                unsigned code;
                if (!readHex4(code))
                    return false;
                if (code >= 0xD800 && code <= 0xDBFF && m_pos + 6 <= m_text.size() && m_text[m_pos] == '\\' && m_text[m_pos + 1] == 'u') {
                    m_pos += 2;
                    unsigned low;
                    if (!readHex4(low))
                        return false;
                    if (low >= 0xDC00 && low <= 0xDFFF)
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    else {
                        appendUTF8(out, 0xFFFD);
                        code = low;
                    }
                } else if (code >= 0xD800 && code <= 0xDFFF)
                    code = 0xFFFD;
                appendUTF8(out, code);
                break;
            }
            default:
                return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }

    bool parseArray(Value& out, int depth)
    {
        out.m_type = Value::Type::Array;
        ++m_pos;
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        for (;;) {
            out.m_array.emplace_back();
            if (!parseValue(out.m_array.back(), depth + 1))
                return false;
            skipSpace();
            if (m_pos >= m_text.size())
                return fail("unterminated array");
            char c = m_text[m_pos++];
            if (c == ']')
                return true;
            if (c != ',')
                return fail("expected , or ]");
            skipSpace();
            // Allow a trailing comma.
            if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
        }
    }

    bool parseObject(Value& out, int depth)
    {
        out.m_type = Value::Type::Object;
        ++m_pos;
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        for (;;) {
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                return fail("expected a key");
            std::string key;
            if (!parseString(key))
                return false;
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                return fail("expected :");
            ++m_pos;
            out.m_object.emplace_back(std::move(key), Value());
            if (!parseValue(out.m_object.back().second, depth + 1))
                return false;
            skipSpace();
            if (m_pos >= m_text.size())
                return fail("unterminated object");
            char c = m_text[m_pos++];
            if (c == '}')
                return true;
            if (c != ',')
                return fail("expected , or }");
            skipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
        }
    }

    const std::string& m_text;
    size_t m_pos { 0 };
    std::string m_error;
};

bool parse(const std::string& text, Value& out, std::string* error)
{
    out = Value();
    Parser parser(text);
    return parser.run(out, error);
}

static void serializeInto(const Value& value, std::string& out)
{
    switch (value.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += value.asBool() ? "true" : "false"; break;
    case Value::Type::Number: {
        char buffer[40];
        double number = value.asNumber();
        if (number != number || number - number != 0) // NaN or infinity
            out += "null";
        else {
            snprintf(buffer, sizeof(buffer), "%.17g", number);
            out += buffer;
        }
        break;
    }
    case Value::Type::String: out += quote(value.asString()); break;
    case Value::Type::Array: {
        out += '[';
        bool first = true;
        for (auto& item : value.items()) {
            if (!first)
                out += ',';
            first = false;
            serializeInto(item, out);
        }
        out += ']';
        break;
    }
    case Value::Type::Object: {
        out += '{';
        bool first = true;
        for (auto& member : value.members()) {
            if (!first)
                out += ',';
            first = false;
            out += quote(member.first);
            out += ':';
            serializeInto(member.second, out);
        }
        out += '}';
        break;
    }
    }
}

std::string serialize(const Value& value)
{
    std::string out;
    serializeInto(value, out);
    return out;
}

std::string quote(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '<': out += "\\u003c"; break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else
                out += static_cast<char>(c);
        }
    }
    out += '"';
    // U+2028/2029 break older JavaScript string literals.
    for (const char* separator : { "\xE2\x80\xA8", "\xE2\x80\xA9" }) {
        size_t pos = 0;
        while ((pos = out.find(separator, pos)) != std::string::npos)
            out.replace(pos, 3, separator[2] == '\xA8' ? "\\u2028" : "\\u2029");
    }
    return out;
}

} // namespace json
