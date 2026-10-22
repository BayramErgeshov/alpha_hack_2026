// Backend unit tests. Run from the project root: backend_tests [filter]
#include "test_harness.hpp"

#include "data_store.hpp"
#include "news.hpp"

#include <cstdlib>
#include <algorithm>
#include <memory>

namespace {
const DataStore& store() {
    static std::unique_ptr<DataStore> instance = [] {
        auto s = std::make_unique<DataStore>();
        std::string error;
        const char* dir = std::getenv("ALFA_DATA_DIR");
        if (!s->load(dir ? dir : "data/input", error)) throw std::runtime_error("dataset: " + error);
        return s;
    }();
    return *instance;
}
} // namespace

TEST(clients_filtered_by_threshold_and_sorted) {
    ClientFilter filter;
    filter.limit = 20;
    const json::Value result = store().list_clients(filter);
    const auto& items = result.get("items").as_array();
    CHECK(!items.empty());
    CHECK(result.get("total").as_number() >= static_cast<double>(items.size()));
    double previous = 1e300;
    for (const auto& item : items) {
        const double revenue = item.get("revenue_rub").as_number();
        CHECK(revenue >= 15e9);
        CHECK(revenue <= previous);
        previous = revenue;
        CHECK(item.get("products").is_array());
    }
    ClientFilter narrowed = filter;
    narrowed.industry = items.front().get("industry").as_string();
    const json::Value subset = store().list_clients(narrowed);
    CHECK(subset.get("total").as_number() <= result.get("total").as_number());
    for (const auto& item : subset.get("items").as_array())
        CHECK(item.get("industry").as_string() == narrowed.industry);
}

TEST(clients_facets_list_industries_and_regions) {
    const json::Value facets = store().client_facets(15e9);
    CHECK(!facets.get("industries").as_array().empty());
    CHECK(!facets.get("regions").as_array().empty());
    CHECK(facets.get("industries").as_array().size() <= 30);
}

TEST(clients_query_matches_id) {
    ClientFilter filter;
    filter.min_revenue = 0;
    filter.query = "U_9250";
    const json::Value result = store().list_clients(filter);
    CHECK(result.get("total").as_number() == 1);
    CHECK(result.get("items").as_array().front().get("id").as_string() == "U_9250");
}

TEST(rss_parses_cdata_entities_and_atom) {
    const std::string rss = R"(<?xml version="1.0" encoding="utf-8"?><rss version="2.0"><channel><title>T</title>
<item><title><![CDATA[ЦБ сохранил ключевую ставку &laquo;14%&raquo;]]></title><link>https://example.ru/a?x=1&amp;y=2</link>
<description><![CDATA[<p>Совет директоров <b>Банка России</b> принял решение.</p>]]></description>
<pubDate>Fri, 11 Sep 2026 13:30:00 +0300</pubDate></item>
<item><title>Нефть &amp; газ: рост</title><link>https://example.ru/b</link><description>Brent &#8212; выше $80</description></item>
</channel></rss>)";
    const auto items = parse_feed_xml(rss, "Тест");
    CHECK(items.size() == 2);
    CHECK(items[0].title == "ЦБ сохранил ключевую ставку «14%»");
    CHECK(items[0].url == "https://example.ru/a?x=1&y=2");
    CHECK(items[0].summary.find("<") == std::string::npos);
    CHECK(items[0].summary.find("Банка России") != std::string::npos);
    CHECK(items[0].published_epoch == 1789122600LL);
    CHECK(items[1].title == "Нефть & газ: рост");
    CHECK(items[1].summary == "Brent — выше $80");
    CHECK(items[1].published_epoch == 0);
    const std::string atom = R"(<feed xmlns="http://www.w3.org/2005/Atom"><entry><title type="html">Курс рубля</title>
<link rel="alternate" href="https://example.ru/c"/><updated>2026-10-20T09:15:00Z</updated><summary>Рубль укрепился.</summary></entry></feed>)";
    const auto atom_items = parse_feed_xml(atom, "Atom");
    CHECK(atom_items.size() == 1);
    CHECK(atom_items[0].url == "https://example.ru/c");
    CHECK(atom_items[0].published_epoch == 1792487700LL);
    CHECK(!atom_items[0].id.empty());
}

TEST(news_dedup_merges_same_story) {
    NewsStore news;
    NewsItem a; a.title = "Банк России сохранил ключевую ставку на уровне 14% годовых"; a.url = "https://a.ru/1"; a.source = "A";
    NewsItem b; b.title = "Банк России сохранил ключевую ставку на уровне 14%"; b.url = "https://b.ru/2"; b.source = "B";
    NewsItem c; c.title = "Цены на никель обновили максимум"; c.url = "https://c.ru/3"; c.source = "C";
    CHECK(news.add(a));
    CHECK(!news.add(b));
    CHECK(news.add(c));
    CHECK(news.size() == 2);
    const auto recent = news.recent(10);
    bool merged = false;
    for (const auto& item : recent) if (item.url == "https://a.ru/1") merged = item.duplicates.size() == 1;
    CHECK(merged);
    NewsItem again = a;
    CHECK(!news.add(again));
    CHECK(news.size() == 2);
}

TEST(classify_rate_news) {
    const auto rate = classify_news("Банк России сохранил ключевую ставку, кредиты остаются дорогими");
    CHECK(std::find(rate.begin(), rate.end(), "rate") != rate.end());
    const auto fx = classify_news("Курс доллара превысил 95 рублей, экспортеры продают валюту");
    CHECK(std::find(fx.begin(), fx.end(), "currency") != fx.end());
    const auto none = classify_news("Футбольный клуб выиграл матч");
    CHECK(none.size() == 1 && none[0] == "other");
}

TEST(news_seed_loads) {
    NewsStore news;
    std::string error;
    CHECK(news.load_seed("data/news/seed.json", error));
    CHECK(news.size() >= 30);
    for (const auto& item : news.recent(100)) {
        CHECK(!item.title.empty());
        CHECK(item.url.rfind("https://", 0) == 0);
        CHECK(!item.categories.empty());
    }
}

int main(int argc, char** argv) { return run_all_tests(argc > 1 ? argv[1] : ""); }
