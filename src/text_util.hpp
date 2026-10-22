#pragma once
// Small UTF-8 helpers shared by the new modules (news, feed, clients).
#include <cstdint>
#include <string>
#include <vector>

namespace textutil {

inline void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7FF) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp <= 0xFFFF) {
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

// Decodes UTF-8 into code points; invalid bytes are passed through as Latin-1.
inline std::vector<uint32_t> decode(const std::string& s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c; size_t n = 1;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        bool ok = i + n <= s.size();
        for (size_t j = 1; ok && j < n; ++j) {
            const unsigned char d = static_cast<unsigned char>(s[i + j]);
            if ((d & 0xC0) != 0x80) ok = false; else cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok) { out.push_back(c); ++i; continue; }
        out.push_back(cp); i += n;
    }
    return out;
}

inline uint32_t lower_cp(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0x410 && cp <= 0x42F) return cp + 32;
    if (cp == 0x401 || cp == 0x451) return 0x435; // ё -> е for matching
    return cp;
}

inline std::string lower(const std::string& s) {
    std::string out;
    for (uint32_t cp : decode(s)) append_utf8(out, lower_cp(cp));
    return out;
}

// Truncates to at most max_bytes without splitting a UTF-8 sequence.
inline std::string truncate_utf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    return s.substr(0, end);
}

inline bool is_word_cp(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 0x430 && cp <= 0x44F) || cp == 0x451;
}

// Lower-cased word tokens (letters/digits), minimum length in code points.
inline std::vector<std::string> words(const std::string& s, size_t min_len = 3) {
    std::vector<std::string> out;
    std::string cur; size_t len = 0;
    for (uint32_t cp : decode(s)) {
        cp = lower_cp(cp);
        if (is_word_cp(cp)) { append_utf8(cur, cp); ++len; }
        else { if (len >= min_len) out.push_back(cur); cur.clear(); len = 0; }
    }
    if (len >= min_len) out.push_back(cur);
    return out;
}

inline bool contains_any(const std::string& lowered, std::initializer_list<const char*> needles) {
    for (const char* n : needles) if (lowered.find(n) != std::string::npos) return true;
    return false;
}

inline bool contains_any(const std::string& lowered, const std::vector<const char*>& needles) {
    for (const char* n : needles) if (lowered.find(n) != std::string::npos) return true;
    return false;
}

// Word-start matching: a needle matches when some word of the text starts with it
// ("сталь" matches "стальной" but not "встал"). Needles with a space or hyphen are
// matched as plain substrings of the lowered text.
class WordIndex {
public:
    explicit WordIndex(const std::string& text) : lowered_(lower(text)), words_(words(text, 1)) {}
    bool any(std::initializer_list<const char*> needles) const { for (const char* n : needles) if (match(n)) return true; return false; }
    bool any(const std::vector<const char*>& needles) const { for (const char* n : needles) if (match(n)) return true; return false; }
    bool match(const char* needle) const {
        const std::string n(needle);
        if (n.find(' ') != std::string::npos || n.find('-') != std::string::npos) return lowered_.find(n) != std::string::npos;
        for (const auto& w : words_) if (w.compare(0, n.size(), n) == 0) return true;
        return false;
    }
    const std::string& lowered() const { return lowered_; }

private:
    std::string lowered_;
    std::vector<std::string> words_;
};

} // namespace textutil
