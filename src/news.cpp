#define _CRT_SECURE_NO_WARNINGS
#include "news.hpp"

#include "reader.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string collapse(const std::string& s) {
    std::string out;
    bool space = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) { if (!out.empty()) space = true; continue; }
        if (space) { out.push_back(' '); space = false; }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string decode_entities(const std::string& s) {
    static const std::pair<const char*, uint32_t> named[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
        { "laquo", 0xAB }, { "raquo", 0xBB }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 },
        { "rsquo", 0x2019 }, { "lsquo", 0x2018 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bdquo", 0x201E },
        { "times", 0xD7 }, { "euro", 0x20AC }, { "copy", 0xA9 }, { "deg", 0xB0 }, { "minus", 0x2212 },
    };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out.push_back(s[i]); continue; }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out.push_back('&'); continue; }
        const std::string name = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (!name.empty() && name[0] == '#') {
            try {
                cp = (name.size() > 1 && (name[1] == 'x' || name[1] == 'X')) ? static_cast<uint32_t>(std::stoul(name.substr(2), nullptr, 16))
                                                                              : static_cast<uint32_t>(std::stoul(name.substr(1)));
            } catch (...) { cp = 0; }
        } else {
            for (const auto& n : named) if (name == n.first) { cp = n.second; break; }
        }
        if (cp == 0 || cp > 0x10FFFF) { out.push_back('&'); continue; }
        textutil::append_utf8(out, cp == 0xA0 ? ' ' : cp);
        i = semi;
    }
    return out;
}

std::string strip_tags(const std::string& s) {
    std::string out;
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') { in_tag = true; out.push_back(' '); continue; }
        if (c == '>') { in_tag = false; continue; }
        if (!in_tag) out.push_back(c);
    }
    return out;
}

// Removes CDATA wrappers, decodes entities, strips markup (also markup that was entity-encoded).
std::string clean_text(std::string raw) {
    std::string unwrapped;
    for (size_t pos = 0;;) {
        const size_t start = raw.find("<![CDATA[", pos);
        if (start == std::string::npos) { unwrapped += raw.substr(pos); break; }
        unwrapped += raw.substr(pos, start - pos);
        const size_t end = raw.find("]]>", start + 9);
        if (end == std::string::npos) { unwrapped += raw.substr(start + 9); break; }
        unwrapped += raw.substr(start + 9, end - start - 9);
        pos = end + 3;
    }
    std::string text = strip_tags(unwrapped);
    text = decode_entities(text);
    if (text.find('<') != std::string::npos) text = decode_entities(strip_tags(text));
    return collapse(trim(text));
}

// Finds the inner text of the first <name ...>...</name> inside block; attrs receives the opening tag.
bool tag_inner(const std::string& block, const std::string& name, std::string& inner, std::string* attrs = nullptr) {
    size_t pos = 0;
    while ((pos = block.find("<" + name, pos)) != std::string::npos) {
        const char next = pos + 1 + name.size() < block.size() ? block[pos + 1 + name.size()] : '\0';
        if (next != '>' && next != ' ' && next != '/' && next != '\t' && next != '\n' && next != '\r') { ++pos; continue; }
        const size_t close = block.find('>', pos);
        if (close == std::string::npos) return false;
        if (attrs) *attrs = block.substr(pos, close - pos + 1);
        if (block[close - 1] == '/') { inner.clear(); return true; }
        const size_t end = block.find("</" + name + ">", close);
        if (end == std::string::npos) return false;
        inner = block.substr(close + 1, end - close - 1);
        return true;
    }
    return false;
}

std::string attribute(const std::string& tag, const std::string& name) {
    const size_t pos = tag.find(name + "=");
    if (pos == std::string::npos) return {};
    const char quote = tag[pos + name.size() + 1];
    if (quote != '"' && quote != '\'') return {};
    const size_t end = tag.find(quote, pos + name.size() + 2);
    if (end == std::string::npos) return {};
    return tag.substr(pos + name.size() + 2, end - pos - name.size() - 2);
}

long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

long long to_epoch(int y, int mo, int d, int h, int mi, int s, int offset_minutes) {
    if (y < 1990 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) return 0;
    return days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400LL + h * 3600LL + mi * 60LL + s - offset_minutes * 60LL;
}

std::string iso_from_epoch(long long epoch) {
    if (epoch <= 0) return {};
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::vector<std::string> stems(const std::string& title) {
    std::vector<std::string> out;
    for (const auto& word : textutil::words(title, 3)) {
        const auto cps = textutil::decode(word);
        std::string stem;
        for (size_t i = 0; i < cps.size() && i < 5; ++i) textutil::append_utf8(stem, cps[i]);
        if (std::find(out.begin(), out.end(), stem) == out.end()) out.push_back(stem);
    }
    return out;
}

double jaccard(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.empty() || b.empty()) return 0;
    size_t common = 0;
    for (const auto& x : a) if (std::find(b.begin(), b.end(), x) != b.end()) ++common;
    return static_cast<double>(common) / static_cast<double>(a.size() + b.size() - common);
}

NewsItem item_from_json(const json::Value& v) {
    NewsItem item;
    item.title = clean_text(v.get("title").as_string());
    item.summary = clean_text(v.get("summary").as_string());
    item.url = trim(v.get("url").as_string());
    item.source = v.get("source").as_string();
    item.published_epoch = parse_news_date(v.get("published").as_string());
    item.id = v.get("id").is_string() ? v.get("id").as_string() : std::string();
    for (const auto& c : v.get("categories").as_array()) if (c.is_string()) item.categories.push_back(c.as_string());
    for (const auto& d : v.get("duplicates").as_array()) if (d.is_object()) item.duplicates.push_back(d);
    return item;
}

} // namespace

std::vector<std::string> classify_news(const std::string& text) {
    const textutil::WordIndex t(text);
    std::vector<std::string> out;
    const auto add = [&](const char* key, std::initializer_list<const char*> needles) {
        if (t.any(needles)) out.push_back(key);
    };
    add("rate", { "ключев", "ставк", "кредит", "ипотек", "заемщик", "облигац", "доходност", "офз", "денежно-кредит" });
    add("currency", { "курс", "рубл", "доллар", "евро", "юан", "валют", "экспорт", "импорт" });
    add("commodity", { "нефт", "газ", "brent", "металл", "сталь", "никел", "алюмин", "медь", "золот", "угол", "угл", "уголь", "зерн", "пшениц", "удобрен", "сырь" });
    add("logistics", { "логист", "перевоз", "фрахт", "порт", "ржд", "контейнер", "поставк", "грузо" });
    add("sanctions", { "санкц", "эмбарго", "ограничени", "запрет на" });
    add("demand", { "спрос", "потреблен", "продаж", "розниц", "инфляц", "цены", "выручк" });
    add("regulation", { "закон", "налог", "ндс", "пошлин", "тариф", "регулир", "минфин", "госдум", "постановлен" });
    add("labor", { "зарплат", "кадр", "безработиц", "рынок труда", "персонал", "мигрант" });
    add("technology", { "искусственн", "нейросет", "цифров", "технолог", "автоматизац", "импортозамещ" });
    add("macro", { "ввп", "экономик", "росстат", "рецесс", "бюджет", "дефицит" });
    if (out.empty()) out.push_back("other");
    return out;
}

const char* news_category_label(const std::string& key) {
    if (key == "rate") return "Ставки и кредитование";
    if (key == "currency") return "Валюта и внешняя торговля";
    if (key == "commodity") return "Сырьевые рынки";
    if (key == "logistics") return "Логистика и поставки";
    if (key == "sanctions") return "Санкции и ограничения";
    if (key == "demand") return "Спрос, цены и потребление";
    if (key == "regulation") return "Регулирование и налоги";
    if (key == "labor") return "Рынок труда";
    if (key == "technology") return "Технологии";
    if (key == "macro") return "Макроэкономика";
    return "Другое";
}

long long parse_news_date(const std::string& raw) {
    const std::string value = trim(raw);
    if (value.empty()) return 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    // ISO-8601: 2026-10-20T09:15:00Z / +03:00 / 2026-10-20
    if (value.size() >= 10 && value[4] == '-' && value[7] == '-') {
        if (std::sscanf(value.c_str(), "%4d-%2d-%2d", &y, &mo, &d) != 3) return 0;
        int offset = 0;
        if (value.size() >= 16 && (value[10] == 'T' || value[10] == ' ')) {
            std::sscanf(value.c_str() + 11, "%2d:%2d", &h, &mi);
            if (value.size() >= 19 && value[16] == ':') std::sscanf(value.c_str() + 17, "%2d", &s);
            const size_t tz = value.find_first_of("+-", 19 <= value.size() ? 19 : value.size());
            if (tz != std::string::npos && tz + 3 <= value.size()) {
                int oh = 0, om = 0;
                std::sscanf(value.c_str() + tz + 1, "%2d", &oh);
                if (tz + 4 < value.size()) std::sscanf(value.c_str() + tz + (value[tz + 3] == ':' ? 4 : 3), "%2d", &om);
                offset = (value[tz] == '-' ? -1 : 1) * (oh * 60 + om);
            }
        }
        return to_epoch(y, mo, d, h, mi, s, offset);
    }
    // RFC 822: Fri, 11 Sep 2026 13:30:00 +0300
    static const char* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    std::string body = value;
    const size_t comma = body.find(',');
    if (comma != std::string::npos) body = trim(body.substr(comma + 1));
    char month[8] = {};
    char zone[16] = {};
    const int parsed = std::sscanf(body.c_str(), "%d %3s %d %d:%d:%d %15s", &d, month, &y, &h, &mi, &s, zone);
    if (parsed < 5) return 0;
    for (int i = 0; i < 12; ++i) if (std::string(month) == months[i]) mo = i + 1;
    int offset = 0;
    const std::string z = zone;
    if (z.size() == 5 && (z[0] == '+' || z[0] == '-')) {
        try { const int hhmm = std::stoi(z.substr(1)); offset = (z[0] == '-' ? -1 : 1) * ((hhmm / 100) * 60 + hhmm % 100); } catch (...) {}
    } else if (z == "MSK") offset = 180;
    return to_epoch(y, mo, d, h, mi, s, offset);
}

std::string news_id_for_url(const std::string& url) {
    unsigned long long hash = 1469598103934665603ULL;
    for (unsigned char c : url) { hash ^= c; hash *= 1099511628211ULL; }
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "n%012llx", hash & 0xFFFFFFFFFFFFULL);
    return buffer;
}

std::vector<NewsItem> parse_feed_xml(const std::string& xml, const std::string& source) {
    std::vector<NewsItem> items;
    const bool atom = xml.find("<entry") != std::string::npos && xml.find("<item") == std::string::npos;
    const std::string open = atom ? "<entry" : "<item";
    const std::string close = atom ? "</entry>" : "</item>";
    size_t pos = 0;
    while (items.size() < 300 && (pos = xml.find(open, pos)) != std::string::npos) {
        const char next = pos + open.size() < xml.size() ? xml[pos + open.size()] : '\0';
        if (next != '>' && next != ' ') { pos += open.size(); continue; }
        const size_t end = xml.find(close, pos);
        if (end == std::string::npos) break;
        const std::string block = xml.substr(pos, end - pos);
        pos = end + close.size();
        NewsItem item;
        item.source = source;
        std::string inner, attrs;
        if (tag_inner(block, "title", inner)) item.title = clean_text(inner);
        if (atom) {
            size_t lp = 0;
            while ((lp = block.find("<link", lp)) != std::string::npos) {
                const size_t le = block.find('>', lp);
                if (le == std::string::npos) break;
                const std::string tag = block.substr(lp, le - lp + 1);
                const std::string rel = attribute(tag, "rel");
                if (rel.empty() || rel == "alternate") { item.url = decode_entities(attribute(tag, "href")); break; }
                lp = le;
            }
            if (tag_inner(block, "summary", inner) || tag_inner(block, "content", inner)) item.summary = clean_text(inner);
            if (tag_inner(block, "published", inner) || tag_inner(block, "updated", inner)) item.published_epoch = parse_news_date(clean_text(inner));
        } else {
            if (tag_inner(block, "link", inner)) item.url = trim(decode_entities(clean_text(inner)));
            if (tag_inner(block, "description", inner)) item.summary = clean_text(inner);
            if (tag_inner(block, "pubDate", inner) || tag_inner(block, "dc:date", inner)) item.published_epoch = parse_news_date(clean_text(inner));
        }
        if (item.title.empty() || item.url.rfind("http", 0) != 0) continue;
        if (item.summary.size() > 1200) item.summary = item.summary.substr(0, 1200);
        item.id = news_id_for_url(item.url);
        item.published = iso_from_epoch(item.published_epoch);
        items.push_back(std::move(item));
    }
    return items;
}

json::Value news_to_json(const NewsItem& item) {
    json::Value v = json::Value::object();
    v["id"] = item.id;
    v["title"] = item.title;
    v["summary"] = item.summary;
    v["url"] = item.url;
    v["source"] = item.source;
    v["published"] = item.published.empty() ? json::Value(nullptr) : json::Value(item.published);
    v["categories"] = json::Value::array();
    v["category_labels"] = json::Value::array();
    for (const auto& c : item.categories) { v["categories"].push_back(c); v["category_labels"].push_back(news_category_label(c)); }
    v["duplicates"] = json::Value::array();
    for (const auto& d : item.duplicates) v["duplicates"].push_back(d);
    return v;
}

bool NewsStore::add_locked(NewsItem item) {
    if (item.title.empty() || item.url.empty()) return false;
    if (item.id.empty()) item.id = news_id_for_url(item.url);
    if (item.published.empty()) item.published = iso_from_epoch(item.published_epoch);
    if (item.categories.empty()) item.categories = classify_news(item.title + " " + item.summary);
    const auto item_stems = stems(item.title);
    for (auto& existing : items_) {
        bool same_url = existing.url == item.url;
        for (const auto& d : existing.duplicates) if (d.get("url").as_string() == item.url) same_url = true;
        if (same_url) return false;
        if (jaccard(stems(existing.title), item_stems) >= 0.6) {
            json::Value copy = json::Value::object();
            copy["url"] = item.url;
            copy["source"] = item.source;
            existing.duplicates.push_back(std::move(copy));
            if (existing.summary.size() < item.summary.size()) existing.summary = item.summary;
            return false;
        }
    }
    items_.push_back(std::move(item));
    if (items_.size() > 2000) items_.erase(items_.begin());
    return true;
}

bool NewsStore::add(NewsItem item) {
    std::lock_guard<std::mutex> lock(mutex_);
    return add_locked(std::move(item));
}

std::vector<NewsItem> NewsStore::recent(size_t limit) const {
    std::vector<NewsItem> copy;
    { std::lock_guard<std::mutex> lock(mutex_); copy = items_; }
    std::stable_sort(copy.begin(), copy.end(), [](const NewsItem& a, const NewsItem& b) { return a.published_epoch > b.published_epoch; });
    if (copy.size() > limit) copy.resize(limit);
    return copy;
}

bool NewsStore::find(const std::string& id, NewsItem& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : items_) if (item.id == id) { out = item; return true; }
    return false;
}

size_t NewsStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

bool NewsStore::load_seed(const std::string& path, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "Cannot open news seed " + path; return false; }
    std::stringstream buffer;
    buffer << input.rdbuf();
    json::Value root;
    try { root = json::Value::parse(buffer.str()); } catch (const std::exception& e) { error = e.what(); return false; }
    const json::Value& list = root.is_array() ? root : root.get("items");
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& v : list.as_array()) add_locked(item_from_json(v));
    return true;
}

void NewsStore::load_state(const std::string& path) {
    std::string ignored;
    load_seed(path, ignored);
}

bool NewsStore::save(const std::string& path) const {
    json::Value list = json::Value::array();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : items_) {
            json::Value v = news_to_json(item);
            v["published"] = item.published;
            list.push_back(std::move(v));
        }
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << list.dump();
        if (!out) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

const std::vector<NewsSource>& default_news_sources() {
    static const std::vector<NewsSource> sources = {
        { "РБК", "https://rssexport.rbc.ru/rbcnews/news/30/full.rss" },
        { "Интерфакс", "https://www.interfax.ru/rss.asp" },
        { "ТАСС", "https://tass.ru/rss/v2.xml" },
        { "Коммерсантъ", "https://www.kommersant.ru/RSS/news.xml" },
        { "Ведомости", "https://www.vedomosti.ru/rss/news" },
        { "Банк России", "https://www.cbr.ru/rss/eventrss" },
    };
    return sources;
}

json::Value NewsCollector::refresh_now() {
    std::lock_guard<std::mutex> guard(refresh_mutex_);
    json::Value report = json::Value::object();
    report["sources"] = json::Value::array();
    long long added_total = 0;
    for (const auto& source : default_news_sources()) {
        json::Value row = json::Value::object();
        row["source"] = source.name;
        std::string body, error;
        if (!fetch_public_text(source.url, 20, body, error)) {
            row["ok"] = false;
            row["error"] = error.substr(0, 200);
            report["sources"].push_back(std::move(row));
            continue;
        }
        long long added = 0;
        const auto items = parse_feed_xml(body, source.name);
        for (const auto& item : items) if (store_.add(item)) ++added;
        added_total += added;
        row["ok"] = true;
        row["parsed"] = static_cast<long long>(items.size());
        row["added"] = added;
        report["sources"].push_back(std::move(row));
    }
    report["added"] = added_total;
    report["total"] = static_cast<long long>(store_.size());
    if (added_total > 0 && !state_path_.empty()) store_.save(state_path_);
    return report;
}

void NewsCollector::start(int minutes) {
    if (minutes <= 0 || worker_.joinable()) return;
    worker_ = std::thread([this, minutes] {
        while (!stop_) {
            try {
                const json::Value report = refresh_now();
                std::cerr << "news refresh added=" << report.get("added").as_number() << " total=" << report.get("total").as_number() << '\n';
            } catch (...) { std::cerr << "news refresh failed\n"; }
            for (int i = 0; i < minutes * 60 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
}

NewsCollector::~NewsCollector() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}
