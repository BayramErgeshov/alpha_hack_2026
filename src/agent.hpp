#pragma once

#include "analytics.hpp"
#include "data_store.hpp"
#include "json.hpp"
#include "llm_client.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

struct ArticleInput {
    std::string url;
    std::string title;
    std::string published_time;
    std::string text;
    std::string source_name;
};

class InsightAgent {
public:
    InsightAgent(const DataStore& data, const AnalyticsIndex& analytics, LlmConfig config);
    json::Value analyze(const std::string& company_id, const ArticleInput& article);
    json::Value follow_up(const std::string& insight_id, const std::string& question);

private:
    struct StoredInsight {
        std::string company_id;
        ArticleInput article;
        json::Value result;
    };
    const DataStore& data_;
    const AnalyticsIndex& analytics_;
    LlmConfig llm_;
    std::mutex mutex_;
    std::unordered_map<std::string, StoredInsight> insights_;

    json::Value rank(const CompanyProfile& company, const ArticleInput& article) const;
};
