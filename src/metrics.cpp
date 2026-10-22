#include "metrics.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
double share(long long part, long long whole) { return whole > 0 ? std::round(1000.0 * part / whole) / 1000.0 : 0.0; }
}

Metrics::Metrics(std::string state_path) : path_(std::move(state_path)) {
    if (path_.empty()) return;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return;
    std::stringstream buffer;
    buffer << in.rdbuf();
    try {
        const json::Value v = json::Value::parse(buffer.str());
        const auto n = [&](const char* k) { return static_cast<long long>(v.get(k).as_number()); };
        insights_ = n("insights"); cached_ = n("cached"); llm_ = n("llm"); revised_ = n("revised");
        bank_position_ = n("bank_position"); claims_total_ = n("claims_total"); claims_supported_ = n("claims_supported");
        latency_sum_ms_ = n("latency_sum_ms"); feedback_ = n("feedback"); useful_ = n("useful"); feeds_ = n("feeds");
        for (const auto& [role, count] : v.get("by_role").as_object()) by_role_[role] = static_cast<long long>(count.as_number());
        for (const auto& r : v.get("reasons").as_array()) reasons_.push_back(r);
    } catch (...) {}
}

void Metrics::record_insight(const json::Value& insight) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++insights_;
    if (insight.get("cached").as_bool()) { ++cached_; save_locked(); return; }
    const json::Value& q = insight.get("quality_check");
    if (q.get("llm_used").as_bool()) ++llm_;
    if (q.get("revise_rounds").as_number() > 0) ++revised_;
    if (!insight.get("interpretation").get("bank_position").is_null()) ++bank_position_;
    claims_total_ += static_cast<long long>(q.get("claims_total").as_number());
    claims_supported_ += static_cast<long long>(q.get("claims_supported").as_number());
    for (const auto& step : insight.get("agent_trace").as_array()) latency_sum_ms_ += static_cast<long long>(step.get("duration_ms").as_number());
    ++by_role_[insight.get("role").get("id").as_string()];
    save_locked();
}

void Metrics::record_feedback(const std::string& insight_id, bool useful, const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++feedback_;
    if (useful) ++useful_;
    if (!reason.empty()) {
        json::Value r = json::Value::object();
        r["insight_id"] = insight_id;
        r["useful"] = useful;
        r["reason"] = reason.substr(0, 500);
        r["at"] = static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        reasons_.push_back(std::move(r));
        while (reasons_.size() > 50) reasons_.pop_front();
    }
    save_locked();
}

void Metrics::record_feed(const std::string& role) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++feeds_;
    (void)role;
}

json::Value Metrics::snapshot(TokenBudget& budget) {
    std::lock_guard<std::mutex> lock(mutex_);
    const long long fresh = insights_ - cached_;
    json::Value v = json::Value::object();
    v["insights"] = json::Value::object();
    v["insights"]["total"] = insights_;
    v["insights"]["cached"] = cached_;
    v["insights"]["llm_share"] = share(llm_, fresh);
    v["insights"]["by_role"] = json::Value::object();
    for (const auto& [role, count] : by_role_) v["insights"]["by_role"][role] = count;
    v["feeds_served"] = feeds_;
    v["quality"] = json::Value::object();
    v["quality"]["supported_claims_share"] = share(claims_supported_, claims_total_);
    v["quality"]["revise_share"] = share(revised_, llm_);
    v["quality"]["bank_position_share"] = share(bank_position_, fresh);
    v["feedback"] = json::Value::object();
    v["feedback"]["total"] = feedback_;
    v["feedback"]["useful_share"] = share(useful_, feedback_);
    v["feedback"]["recent_reasons"] = json::Value::array();
    for (const auto& r : reasons_) v["feedback"]["recent_reasons"].push_back(r);
    v["latency"] = json::Value::object();
    v["latency"]["avg_ms"] = fresh > 0 ? latency_sum_ms_ / fresh : 0;
    v["budget"] = budget.stats();
    v["definitions"] = json::Value::object();
    v["definitions"]["supported_claims_share"] = "Доля утверждений разбора со ссылкой на существующий источник";
    v["definitions"]["revise_share"] = "Доля разборов моделью, где самопроверка потребовала доработки";
    v["definitions"]["bank_position_share"] = "Доля разборов с подтверждённой позицией Альфа-Банка";
    v["definitions"]["useful_share"] = "Доля оценок «полезно» от пользователей";
    return v;
}

void Metrics::save_locked() const {
    if (path_.empty()) return;
    json::Value v = json::Value::object();
    v["insights"] = insights_; v["cached"] = cached_; v["llm"] = llm_; v["revised"] = revised_;
    v["bank_position"] = bank_position_; v["claims_total"] = claims_total_; v["claims_supported"] = claims_supported_;
    v["latency_sum_ms"] = latency_sum_ms_; v["feedback"] = feedback_; v["useful"] = useful_; v["feeds"] = feeds_;
    v["by_role"] = json::Value::object();
    for (const auto& [role, count] : by_role_) v["by_role"][role] = count;
    v["reasons"] = json::Value::array();
    for (const auto& r : reasons_) v["reasons"].push_back(r);
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << v.dump();
    }
    std::remove(path_.c_str());
    std::rename(tmp.c_str(), path_.c_str());
}
