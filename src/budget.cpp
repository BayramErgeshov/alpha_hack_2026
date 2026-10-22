#include "budget.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
long long utc_day() {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return now / 86400;
}
}

TokenBudget::TokenBudget(long long daily_limit_tokens, std::string state_path)
    : limit_(daily_limit_tokens > 0 ? daily_limit_tokens : 1), path_(std::move(state_path)), day_(utc_day()) {
    if (path_.empty()) return;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return;
    std::stringstream buffer;
    buffer << in.rdbuf();
    try {
        const json::Value v = json::Value::parse(buffer.str());
        if (static_cast<long long>(v.get("day").as_number()) != day_) return;
        used_ = static_cast<long long>(v.get("used_tokens").as_number());
        calls_ = static_cast<long long>(v.get("calls").as_number());
        for (const auto& [model, tokens] : v.get("by_model").as_object()) by_model_[model] = static_cast<long long>(tokens.as_number());
    } catch (...) {}
}

void TokenBudget::roll_day_locked() {
    const long long today = utc_day();
    if (today == day_) return;
    day_ = today;
    used_ = 0;
    calls_ = 0;
    by_model_.clear();
}

bool TokenBudget::allow() {
    std::lock_guard<std::mutex> lock(mutex_);
    roll_day_locked();
    return used_ < limit_;
}

void TokenBudget::record(long long prompt_tokens, long long completion_tokens, const std::string& model) {
    std::lock_guard<std::mutex> lock(mutex_);
    roll_day_locked();
    const long long total = std::max(0LL, prompt_tokens) + std::max(0LL, completion_tokens);
    used_ += total;
    ++calls_;
    by_model_[model] += total;
    save_locked();
}

json::Value TokenBudget::stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    roll_day_locked();
    json::Value v = json::Value::object();
    v["day"] = day_;
    v["used_tokens"] = used_;
    v["limit_tokens"] = limit_;
    v["remaining_tokens"] = std::max(0LL, limit_ - used_);
    v["calls"] = calls_;
    v["exhausted"] = used_ >= limit_;
    v["reset"] = "03:00 MSK";
    json::Value models = json::Value::object();
    for (const auto& [model, tokens] : by_model_) models[model] = tokens;
    v["by_model"] = std::move(models);
    return v;
}

void TokenBudget::save_locked() const {
    if (path_.empty()) return;
    json::Value v = json::Value::object();
    v["day"] = day_;
    v["used_tokens"] = used_;
    v["calls"] = calls_;
    json::Value models = json::Value::object();
    for (const auto& [model, tokens] : by_model_) models[model] = tokens;
    v["by_model"] = std::move(models);
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << v.dump();
    }
    std::remove(path_.c_str());
    std::rename(tmp.c_str(), path_.c_str());
}
