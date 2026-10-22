#pragma once
// Daily token budget for the shared LLM key (organizers' limit is per key and per day,
// reset at 03:00 Moscow time = 00:00 UTC). Thread-safe; optionally persisted to a JSON file.
#include "json.hpp"

#include <map>
#include <mutex>
#include <string>

class TokenBudget {
public:
    TokenBudget(long long daily_limit_tokens, std::string state_path);
    bool allow();                       // false when today's usage reached the limit
    void record(long long prompt_tokens, long long completion_tokens, const std::string& model);
    json::Value stats();

private:
    void roll_day_locked();
    void save_locked() const;
    std::mutex mutex_;
    long long limit_;
    std::string path_;
    long long day_ = 0;
    long long used_ = 0;
    long long calls_ = 0;
    std::map<std::string, long long> by_model_;
};
