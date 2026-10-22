#pragma once

#include "analytics.hpp"
#include "budget.hpp"
#include "data_store.hpp"
#include "json.hpp"
#include "llm_client.hpp"
#include "news.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ArticleInput {
    std::string url;
    std::string title;
    std::string published_time;
    std::string text;
    std::string source_name;
    std::string news_id;                  // set when the article comes from the news store
    std::vector<std::string> categories;  // news category keys; classified when empty
};

// Agentic analysis of one event for one company and one user role:
// plan -> tools -> draft -> verify -> revise (at most one extra round) -> finalize.
// Every step is recorded in agent_trace. Without a model key or budget the same
// pipeline runs with deterministic fallbacks and says so in the trace.
class InsightAgent {
public:
    InsightAgent(const DataStore& data, const AnalyticsIndex& analytics, LlmConfig config, TokenBudget& budget);
    void attach_news(const NewsStore* news) { news_ = news; }
    json::Value analyze(const std::string& company_id, const ArticleInput& article, const std::string& role_id = "ceo");
    // lens: risks | counterparties | treasury | scenarios | sources (optional, replaces or extends the question)
    json::Value follow_up(const std::string& insight_id, const std::string& question, const std::string& lens = "");
    TokenBudget& budget() { return budget_; }
    bool find_insight(const std::string& insight_id, json::Value& out);

private:
    struct StoredInsight {
        std::string company_id;
        ArticleInput article;
        json::Value result;
    };
    const DataStore& data_;
    const AnalyticsIndex& analytics_;
    LlmConfig llm_;
    TokenBudget& budget_;
    const NewsStore* news_ = nullptr;
    std::mutex mutex_;
    std::unordered_map<std::string, StoredInsight> insights_;
    std::unordered_map<std::string, std::string> cache_; // (company|role|article hash) -> insight id

    // Calls a model if the key is configured and the daily budget allows it; records token usage.
    // status receives "complete", "fallback", "budget_exhausted" or "not_configured".
    LlmReply call_model(const std::string& model, const std::string& system, const std::string& user,
        int max_tokens, std::string& status);
};
