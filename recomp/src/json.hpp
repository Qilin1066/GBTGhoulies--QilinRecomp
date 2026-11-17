// Minimal streaming JSON writer (no dependencies). Output only.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace qilin {

class JsonWriter {
public:
    explicit JsonWriter(bool pretty = false) : pretty_(pretty) {}

    JsonWriter& begin_object() { value_prefix(); out_ += '{'; stack_.push_back(true); first_ = true; return *this; }
    JsonWriter& end_object() { close('}'); return *this; }
    JsonWriter& begin_array() { value_prefix(); out_ += '['; stack_.push_back(false); first_ = true; return *this; }
    JsonWriter& end_array() { close(']'); return *this; }

    JsonWriter& key(std::string_view k) {
        if (!first_) out_ += ',';
        newline();
        string_raw(k);
        out_ += pretty_ ? ": " : ":";
        first_ = true;
        after_key_ = true;
        return *this;
    }

    JsonWriter& value(std::string_view s) { value_prefix(); string_raw(s); return *this; }
    JsonWriter& value(const char* s) { return value(std::string_view(s)); }
    JsonWriter& value(const std::string& s) { return value(std::string_view(s)); }
    JsonWriter& value(bool b) { value_prefix(); out_ += b ? "true" : "false"; return *this; }
    JsonWriter& value(int64_t v) { value_prefix(); out_ += std::to_string(v); return *this; }
    JsonWriter& value(uint64_t v) { value_prefix(); out_ += std::to_string(v); return *this; }
    JsonWriter& value(int v) { return value(int64_t(v)); }
    JsonWriter& value(unsigned v) { return value(uint64_t(v)); }
    JsonWriter& null() { value_prefix(); out_ += "null"; return *this; }
    JsonWriter& hex32(uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof b, "%08X", v);
        return value(std::string_view(b));
    }

    template <class T> JsonWriter& kv(std::string_view k, const T& v) { key(k); return value(v); }

    const std::string& str() const { return out_; }

private:
    void value_prefix() {
        if (after_key_) { after_key_ = false; first_ = false; return; }
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
                    if (c < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof b, "\\u%04x", c);
                        out_ += b;
                    } else {
                        out_ += char(c);
                    }
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
