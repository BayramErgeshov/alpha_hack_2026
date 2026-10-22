#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct AnalyticsHit {
    std::string id;
    std::string document;
    int page = 0;
    std::string text;
    double score = 0;
    std::string extraction_method;
};

class AnalyticsIndex {
public:
    bool load(const std::string& directory, std::string& error);
    size_t document_count() const { return documents_.size(); }
    size_t chunk_count() const { return chunks_.size(); }
    std::vector<AnalyticsHit> search(const std::string& query, size_t limit = 5) const;

private:
    struct Chunk {
        std::string id;
        std::string document;
        int page = 0;
        std::string text;
        std::string extraction_method;
        std::unordered_map<std::string, int> terms;
        size_t length = 0;
    };
    std::vector<std::string> documents_;
    std::vector<Chunk> chunks_;
    std::unordered_map<std::string, size_t> document_frequency_;
    double average_length_ = 1;
};
