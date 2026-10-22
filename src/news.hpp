#pragma once
// News storage, RSS/Atom parsing, rule-based classification and background collection.
#include "json.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct NewsItem {
    std::string id;          // stable id derived from the URL
    std::string title;
    std::string summary;     // plain text, no markup
    std::string url;
    std::string source;
    std::string published;   // ISO-8601 UTC or empty
    long long published_epoch = 0; // 0 = unknown
    std::vector<std::string> categories;
    std::vector<json::Value> duplicates; // [{url, source}] of merged copies
};

// Category keys: rate, currency, commodity, logistics, sanctions, demand,
// regulation, labor, technology, macro; "other" when nothing matched.
std::vector<std::string> classify_news(const std::string& text);
const char* news_category_label(const std::string& key);

// Parses RSS 2.0 or Atom. Invalid entries (no title or no link) are skipped.
std::vector<NewsItem> parse_feed_xml(const std::string& xml, const std::string& source);
long long parse_news_date(const std::string& value); // RFC 822 or ISO-8601, 0 if unknown
std::string news_id_for_url(const std::string& url);
json::Value news_to_json(const NewsItem& item);

class NewsStore {
public:
    bool load_seed(const std::string& path, std::string& error);
    void load_state(const std::string& path);
    bool save(const std::string& path) const;
    // Returns false when the item duplicates a stored story (it is merged into it).
    bool add(NewsItem item);
    std::vector<NewsItem> recent(size_t limit) const;
    bool find(const std::string& id, NewsItem& out) const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<NewsItem> items_;
    bool add_locked(NewsItem item);
};

struct NewsSource { std::string name; std::string url; };
const std::vector<NewsSource>& default_news_sources();

class NewsCollector {
public:
    explicit NewsCollector(NewsStore& store, std::string state_path) : store_(store), state_path_(std::move(state_path)) {}
    ~NewsCollector();
    void start(int minutes);
    // manual=true is a user-triggered refresh (HTTP); it is throttled to one per 60 seconds.
    json::Value refresh_now(bool manual = false);

private:
    NewsStore& store_;
    std::string state_path_;
    std::mutex refresh_mutex_;
    std::atomic<bool> stop_{ false };
    long long last_manual_ = 0;
    std::thread worker_;
};
