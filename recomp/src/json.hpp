// Minimal streaming JSON writer (no dependencies). Output only.
#pragma once

#include <concepts>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace qilin {

class JsonWriter {
public:
    explicit JsonWriter(bool pretty = false) : pretty_(pretty) {}

    JsonWriter& begin_object() { open('{', true); return *this; }
    JsonWriter& end_object() { close('}'); return *this; }
    JsonWriter& begin_array() { open('[', false); return *this; }
    JsonWriter& end_array() { close(']'); return *this; }

    JsonWriter& key(std::string_view k) {
        if (!first_) out_ += ',';
        newline();
        string_raw(k);
        out_ += pretty_ ? ": " : ":";
        after_key_ = true;
        return *this;
    }

    JsonWriter& value(std::string_view s) { prefix(); string_raw(s); return *this; }
    JsonWriter& value(const char* s) { return value(std::string_view(s)); }
    JsonWriter& value(const std::string& s) { return value(std::string_view(s)); }
    JsonWriter& value(bool b) { prefix(); out_ += b ? "true" : "false"; return *this; }
    // Any integer type, printed as a number (uint8_t too, never as a character).
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    JsonWriter& value(T v) {
        prefix();
        std::format_to(std::back_inserter(out_), "{}", +v);
        return *this;
    }
    JsonWriter& null() { prefix(); out_ += "null"; return *this; }
    JsonWriter& hex32(uint32_t v) { return value(std::string_view(std::format("{:08X}", v))); }

    template <class T> JsonWriter& kv(std::string_view k, const T& v) { key(k); return value(v); }

    const std::string& str() const { return out_; }

private:
    void open(char c, bool object) {
        prefix();
        out_ += c;
        stack_.push_back(object);
        first_ = true;
    }
    void prefix() {
        if (after_key_) {
            after_key_ = false;
            first_ = false;
            return;
        }
        if (!stack_.empty()) {
            if (!first_) out_ += ',';
            newline();
        }
        first_ = false;
    }
    void close(char c) {
        stack_.pop_back();
        if (!first_) newline();
        out_ += c;
        first_ = false;
    }
    void newline() {
        if (!pretty_) return;
        out_ += '\n';
        out_.append(stack_.size() * 2, ' ');
    }
    void string_raw(std::string_view s) {
        out_ += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (c < 0x20) std::format_to(std::back_inserter(out_), "\\u{:04x}", c);
                    else out_ += char(c);
            }
        }
        out_ += '"';
    }

    bool pretty_;
    bool first_ = true;
    bool after_key_ = false;
    std::vector<bool> stack_;
    std::string out_;
};

}  // namespace qilin
