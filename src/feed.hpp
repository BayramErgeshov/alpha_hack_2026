#pragma once
// Personal news feed: rule-based relevance of stored news for one company and one user role.
// No model calls: the feed is free and instant; deep analysis happens on demand in InsightAgent.
#include "analytics.hpp"
#include "data_store.hpp"
#include "json.hpp"
#include "news.hpp"

#include <map>
#include <string>
#include <vector>

struct RoleInfo {
    std::string id;
    std::string title;
    std::string focus;                     // what this role cares about, used in prompts
    std::map<std::string, double> weights; // news category -> priority multiplier
};

const std::vector<RoleInfo>& roles();
const RoleInfo* find_role(const std::string& id);
const RoleInfo& role_or_default(const std::string& id); // falls back to "ceo"
json::Value roles_json();

// Relevance of one event for one company. Categories are news category keys (see classify_news).
// Result: {score 0..1, band, reasons[], event_categories[], industry_match, counterparty_match, role_boost}
json::Value score_relevance(const CompanyProfile& company, const std::string& title, const std::string& text,
    const std::vector<std::string>& categories, const std::vector<std::string>& counterparty_industries,
    const RoleInfo& role);

// Known gaps in the company profile that limit personalization (human-readable).
std::vector<std::string> profile_gaps(const CompanyProfile& company);

// Two publications about the same event (retellings by different outlets within 36 hours).
// Shared word stems: >= 3 and >= 60% of the shorter text, and the shared words must be rare in the
// current news corpus — titles need one stem found in <= 2 news, title + lead need two stems found in
// <= 3 news. Rarity is counted in `corpus` around the pair's time (+/- 36 h), thresholds scaled to
// the number of news in that window (calibrated on ~500). The feed passes all its candidate news.
// Common words ("прибыль упала", "аэропорт ограничил") alone never join two events.
// Calibrated on the live feed (500 news): joins retellings, keeps related-but-different news apart.
bool same_story(const NewsItem& a, const NewsItem& b, const std::vector<NewsItem>& corpus);

json::Value build_feed(const DataStore& data, const AnalyticsIndex& analytics, const NewsStore& news,
    const CompanyProfile& company, const std::string& role_id, size_t limit);
