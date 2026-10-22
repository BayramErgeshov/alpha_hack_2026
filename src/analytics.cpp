#include "analytics.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_set>

namespace {

bool is_word_codepoint(uint32_t cp) {
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9')) return true;
    if ((cp >= 0x00C0 && cp <= 0x02AF) || (cp >= 0x0370 && cp <= 0x052F)) return true;
    return false;
}

std::string encode_utf8(uint32_t cp) {
    std::string out;
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
    return out;
}

uint32_t lower_codepoint(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0x410 && cp <= 0x42F) return cp + 32;
    if (cp == 0x401) return 0x451;
    if (cp >= 0x391 && cp <= 0x3AB && cp != 0x3A2) return cp + 32;
    return cp;
}

std::vector<std::string> tokenize(const std::string& text) {
    static const std::unordered_set<std::string> stop = {
        "и","в","во","на","не","что","с","со","по","к","от","до","за","из","у","о","об","для","как","а","но","или","это","при","над","под","the","and","of","to","in","on","for","with","by","from","an","a","is","are"
    };
    std::vector<std::string> result;
    std::string token;
    for (size_t i = 0; i < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        uint32_t cp = 0;
        size_t bytes = 1;
        if (first < 0x80) cp = first;
        else if ((first & 0xE0) == 0xC0 && i + 1 < text.size()) { cp = first & 0x1F; bytes = 2; }
        else if ((first & 0xF0) == 0xE0 && i + 2 < text.size()) { cp = first & 0x0F; bytes = 3; }
        else if ((first & 0xF8) == 0xF0 && i + 3 < text.size()) { cp = first & 0x07; bytes = 4; }
        else { ++i; continue; }
        bool valid = true;
        for (size_t j = 1; j < bytes; ++j) {
            const unsigned char next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (next & 0x3F);
        }
        if (!valid) { ++i; continue; }
        cp = lower_codepoint(cp);
        if (is_word_codepoint(cp)) token += encode_utf8(cp);
        else if (!token.empty()) {
            if (!stop.count(token) && token.size() > 1) result.push_back(std::move(token));
            token.clear();
        }
        i += bytes;
    }
    if (!token.empty() && !stop.count(token) && token.size() > 1) result.push_back(std::move(token));
    return result;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

std::vector<std::pair<int, std::string>> pages_from_text(const std::string& text) {
    std::vector<std::pair<int, std::string>> pages;
    const std::string marker = "[[PAGE:";
    size_t cursor = text.find(marker);
    if (cursor == std::string::npos) {
        if (!text.empty() && text.find("TEXT_UNAVAILABLE") == std::string::npos)
            pages.emplace_back(1, text);
        return pages;
    }
    // Preamble contains filename and extraction metadata, not bank evidence.
    while (cursor != std::string::npos) {
        const size_t close = text.find("]]", cursor + marker.size());
        if (close == std::string::npos) break;
        const size_t next = text.find(marker, close + 2);
        std::string body = text.substr(close + 2, next == std::string::npos ? std::string::npos : next - close - 2);
        int page = 0;
        try { page = std::stoi(text.substr(cursor + marker.size(), close - cursor - marker.size())); }
        catch (...) {}
        if (page > 0 && body.find("TEXT_UNAVAILABLE") == std::string::npos &&
            body.find_first_not_of(" \r\n\t") != std::string::npos)
            pages.emplace_back(page, std::move(body));
        cursor = next;
    }
    return pages;
}

std::string extraction_method(const std::string& text) {
    const std::string marker = "<!-- extraction:";
    const size_t begin = text.find(marker);
    if (begin == std::string::npos) return "provided_text";
    const size_t end = text.find("-->", begin + marker.size());
    if (end == std::string::npos) return "provided_text";
    const auto value = text.substr(begin + marker.size(), end - begin - marker.size());
    const size_t first = value.find_first_not_of(" \r\n\t");
    const size_t last = value.find_last_not_of(" \r\n\t");
    return first == std::string::npos ? "provided_text" : value.substr(first, last - first + 1);
}

std::vector<std::string> split_chunks(const std::string& text, size_t max_bytes = 4200) {
    std::vector<std::string> chunks;
    size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
        if (pos >= text.size()) break;
        size_t end = std::min(text.size(), pos + max_bytes);
        if (end < text.size()) {
            size_t split = text.rfind("\n\n", end);
            if (split == std::string::npos || split <= pos + max_bytes / 2)
                split = text.find_last_of(" \n\t", end);
            if (split != std::string::npos && split > pos + max_bytes / 2) end = split;
            while (end > pos && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
        }
        if (end <= pos) end = std::min(text.size(), pos + max_bytes);
        chunks.push_back(text.substr(pos, end - pos));
        pos = end;
    }
    return chunks;
}

} // namespace

bool AnalyticsIndex::load(const std::string& directory, std::string& error) {
    documents_.clear();
    chunks_.clear();
    document_frequency_.clear();
    average_length_ = 1;
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) return true;

    size_t total_terms = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const auto extension = entry.path().extension().string();
        if (extension != ".txt" && extension != ".md") continue;
        if (entry.file_size(ec) > 25 * 1024 * 1024) continue;
        std::string content = read_text(entry.path());
        if (content.empty()) continue;
        if (content.find("OCR_REQUIRED:") != std::string::npos) continue;
        const auto filename = entry.path().filename().u8string();
        const std::string document(filename.begin(), filename.end());
        const std::string method = extraction_method(content);
        auto pages = pages_from_text(content);
        size_t chunk_no = 0;
        for (const auto& page : pages) {
            for (const auto& text : split_chunks(page.second)) {
                Chunk chunk;
                chunk.document = document;
                chunk.page = page.first;
                chunk.text = text;
                chunk.extraction_method = method;
                const auto words = tokenize(text);
                if (words.empty()) continue;
                chunk.id = document + "#page=" + std::to_string(page.first) + "#part=" + std::to_string(++chunk_no);
                chunk.length = words.size();
                std::unordered_set<std::string> unique;
                for (const auto& word : words) ++chunk.terms[word];
                for (const auto& word : chunk.terms) unique.insert(word.first);
                for (const auto& term : unique) ++document_frequency_[term];
                total_terms += chunk.length;
                chunks_.push_back(std::move(chunk));
            }
        }
        if (chunk_no > 0) documents_.push_back(document);
    }
    if (ec) { error = "Could not read analytics directory: " + ec.message(); return false; }
    if (!chunks_.empty()) average_length_ = static_cast<double>(total_terms) / chunks_.size();
    return true;
}

std::vector<AnalyticsHit> AnalyticsIndex::search(const std::string& query, size_t limit) const {
    const auto terms = tokenize(query);
    if (terms.empty() || chunks_.empty() || limit == 0) return {};
    std::vector<std::pair<double, size_t>> ranked;
    ranked.reserve(chunks_.size());
    const double corpus_size = static_cast<double>(chunks_.size());
    for (size_t i = 0; i < chunks_.size(); ++i) {
        const Chunk& chunk = chunks_[i];
        double score = 0;
        for (const auto& term : terms) {
            auto tf_it = chunk.terms.find(term);
            if (tf_it == chunk.terms.end()) continue;
            auto df_it = document_frequency_.find(term);
            const double df = df_it == document_frequency_.end() ? 0 : static_cast<double>(df_it->second);
            const double idf = std::log(1.0 + (corpus_size - df + 0.5) / (df + 0.5));
            const double tf = tf_it->second;
            const double norm = tf + 1.5 * (1.0 - 0.75 + 0.75 * chunk.length / std::max(average_length_, 1.0));
            score += idf * tf * 2.5 / norm;
        }
        if (score > 0) ranked.emplace_back(score, i);
    }
    const size_t take = std::min(limit, ranked.size());
    std::partial_sort(ranked.begin(), ranked.begin() + take, ranked.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<AnalyticsHit> result;
    result.reserve(take);
    for (size_t i = 0; i < take; ++i) {
        const auto& chunk = chunks_[ranked[i].second];
        result.push_back({ chunk.id, chunk.document, chunk.page, chunk.text, ranked[i].first, chunk.extraction_method });
    }
    return result;
}
