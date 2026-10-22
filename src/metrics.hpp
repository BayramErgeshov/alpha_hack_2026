#pragma once
// Quality metrics of content, personalization and AI components, plus user feedback.
#include "budget.hpp"
#include "json.hpp"

#include <deque>
#include <map>
#include <mutex>
#include <string>

class Metrics {
public:
    explicit Metrics(std::string state_path);
    void record_insight(const json::Value& insight);
    void record_feedback(const std::string& insight_id, bool useful, const std::string& reason);
    void record_feed(const std::string& role);
    json::Value snapshot(TokenBudget& budget);

private:
    void save_locked() const;
    std::mutex mutex_;
    std::string path_;
    long long insights_ = 0, cached_ = 0, llm_ = 0, revised_ = 0, bank_position_ = 0;
    long long claims_total_ = 0, claims_supported_ = 0, latency_sum_ms_ = 0;
    long long feedback_ = 0, useful_ = 0, feeds_ = 0;
    std::map<std::string, long long> by_role_;
    std::deque<json::Value> reasons_;
};
