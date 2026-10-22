// Backend unit tests. Run from the project root: backend_tests [filter]
#include "test_harness.hpp"

#include "data_store.hpp"
#include "news.hpp"
#include "feed.hpp"
#include "analytics.hpp"
#include "budget.hpp"
#include "verifier.hpp"

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
const AnalyticsIndex& analytics() {
    static std::unique_ptr<AnalyticsIndex> instance = [] {
        auto a = std::make_unique<AnalyticsIndex>();
        std::string error;
        if (!a->load("data/analytics", error)) throw std::runtime_error("analytics: " + error);
        return a;
    }();
    return *instance;
}

void seed(NewsStore& news) {
    std::string error;
    if (!news.load_seed("data/news/seed.json", error)) throw std::runtime_error(error);
}

CompanyProfile profile(const std::string& id, const std::string& industry, bool loans, bool fts) {
    CompanyProfile p;
    p.id = id; p.industry = industry; p.region = "Москва"; p.revenue = 4e10; p.loans = loans; p.fts = fts;
    return p;
}

std::vector<std::string> feed_ids(const json::Value& feed) {
    std::vector<std::string> ids;
    for (const auto& item : feed.get("items").as_array()) ids.push_back(item.get("news_id").as_string());
    return ids;
}

bool has_category(const json::Value& item, const std::string& key) {
    for (const auto& c : item.get("categories").as_array()) if (c.as_string() == key) return true;
    return false;
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

TEST(feed_changes_with_company) {
    NewsStore news; seed(news);
    const CompanyProfile coal = profile("T_COAL", "Добыча и обогащение угля", false, true);
    const CompanyProfile builder = profile("T_BUILD", "Строительство жилых и нежилых зданий", true, false);
    const json::Value a = build_feed(store(), analytics(), news, coal, "ceo", 10);
    const json::Value b = build_feed(store(), analytics(), news, builder, "ceo", 10);
    CHECK(a.get("items").as_array().size() == 10);
    CHECK(feed_ids(a) != feed_ids(b));
    const auto& top_coal = a.get("items").as_array().front();
    CHECK(top_coal.get("title").as_string().find("гол") != std::string::npos || has_category(top_coal, "commodity"));
    const auto& top_builder = b.get("items").as_array().front();
    CHECK(has_category(top_builder, "rate") || top_builder.get("title").as_string().find("новострой") != std::string::npos);
    CHECK(!top_builder.get("why_for_you").as_string().empty());
}

TEST(feed_changes_with_role) {
    NewsStore news; seed(news);
    const CompanyProfile pipes = profile("T_PIPE", "Производство стальных труб", true, true);
    const json::Value treasurer = build_feed(store(), analytics(), news, pipes, "treasurer", 31);
    const json::Value ceo = build_feed(store(), analytics(), news, pipes, "ceo", 31);
    CHECK(feed_ids(treasurer) != feed_ids(ceo));
    const auto first_currency = [](const json::Value& feed) {
        const auto& items = feed.get("items").as_array();
        for (size_t i = 0; i < items.size(); ++i) if (has_category(items[i], "currency")) return i;
        return items.size();
    };
    CHECK(first_currency(treasurer) <= first_currency(ceo));
    CHECK(treasurer.get("role").get("id").as_string() == "treasurer");
}

TEST(feed_handles_missing_dates) {
    NewsStore news; seed(news);
    NewsItem undated;
    undated.title = "Банк России изменил требования к кредитам строительным компаниям";
    undated.url = "https://example.ru/undated";
    undated.source = "Тест";
    news.add(undated);
    const CompanyProfile builder = profile("T_BUILD", "Строительство жилых и нежилых зданий", true, false);
    const json::Value feed = build_feed(store(), analytics(), news, builder, "cfo", 40);
    const auto& items = feed.get("items").as_array();
    size_t position = items.size();
    for (size_t i = 0; i < items.size(); ++i) if (items[i].get("url").as_string() == "https://example.ru/undated") position = i;
    CHECK(position < items.size() - 1);
    CHECK(items[position].get("published").is_null());
}

TEST(feed_company_with_blank_fields) {
    NewsStore news; seed(news);
    CompanyProfile blank;
    blank.id = "T_BLANK";
    const json::Value feed = build_feed(store(), analytics(), news, blank, "owner", 5);
    CHECK(feed.get("items").as_array().size() == 5);
    bool unknown_noted = false;
    for (const auto& note : feed.get("data_gaps").as_array()) if (!note.as_string().empty()) unknown_noted = true;
    CHECK(unknown_noted);
}

TEST(roles_have_five_entries) {
    CHECK(roles().size() == 5);
    CHECK(find_role("cfo") != nullptr);
    CHECK(find_role("nobody") == nullptr);
}

TEST(matching_uses_word_starts_not_substrings) {
    const auto shop = classify_news("Сеть магазинов открыла новый ресурс для покупателей");
    CHECK(std::find(shop.begin(), shop.end(), "commodity") == shop.end());
    CHECK(std::find(shop.begin(), shop.end(), "currency") == shop.end());
    NewsStore news; seed(news);
    const CompanyProfile retail = profile("T_SHOP", "Торговля розничная в неспециализированных магазинах", false, false);
    const json::Value rel = score_relevance(retail, "Экспорт угля встал", "Вывоз угля через порты встал с августа",
        classify_news("Экспорт угля встал"), {}, role_or_default("ceo"));
    CHECK(rel.get("direct_industry_topic").is_null());
    const CompanyProfile metal = profile("T_MET", "Торговля оптовая металлами и металлическими рудами", false, false);
    const json::Value rel2 = score_relevance(metal, "Экспорт угля встал", "Вывоз через порты встал с августа",
        classify_news("Экспорт угля встал"), {}, role_or_default("ceo"));
    CHECK(rel2.get("direct_industry_topic").is_null());
}

TEST(budget_blocks_after_limit) {
    TokenBudget budget(1000, "");
    CHECK(budget.allow());
    budget.record(600, 300, "deepseek/deepseek-v4.1-flash");
    CHECK(budget.allow());
    budget.record(100, 50, "google/gemma-4-26b-a4b-it");
    CHECK(!budget.allow());
    const json::Value stats = budget.stats();
    CHECK(stats.get("used_tokens").as_number() == 1050);
    CHECK(stats.get("limit_tokens").as_number() == 1000);
    CHECK(stats.get("calls").as_number() == 2);
    CHECK(stats.get("by_model").get("google/gemma-4-26b-a4b-it").as_number() == 150);
    CHECK(stats.get("exhausted").as_bool());
}

TEST(verifier_flags_claim_without_source) {
    const json::Value draft = json::Value::parse(R"({
      "why_it_matters": {"text": "Ставка влияет на долг компании", "evidence_ids": ["N1","C1"]},
      "risks": [{"text": "Рост процентных расходов", "evidence_ids": []},
                {"text": "Сложнее рефинансировать", "evidence_ids": ["X9"]}],
      "opportunities": [{"text": "Зафиксировать ставку", "evidence_ids": ["A1"]}],
      "bank_view": {"text": "Банк ждёт паузы", "evidence_ids": ["N1"]}
    })");
    const json::Value report = verify_claims(draft, { "N1", "C1", "A1" }, "ключевая ставка 14% годовых");
    CHECK(!report.get("ok").as_bool());
    int no_evidence = 0, bank = 0;
    for (const auto& issue : report.get("issues").as_array()) {
        if (issue.get("code").as_string() == "no_evidence") ++no_evidence;
        if (issue.get("code").as_string() == "bank_view_without_bank_source") ++bank;
    }
    CHECK(no_evidence == 2);
    CHECK(bank == 1);
    CHECK(report.get("claims_total").as_number() == 4);
    CHECK(report.get("claims_supported").as_number() == 2);
}

TEST(verifier_flags_number_not_in_sources) {
    const json::Value draft = json::Value::parse(R"({
      "why_it_matters": {"text": "Ставка 14% сохраняет нагрузку", "evidence_ids": ["N1"]},
      "risks": [{"text": "Расходы вырастут на 23,5%", "evidence_ids": ["N1"]}],
      "opportunities": [{"text": "Размещение под 14,0% годовых", "evidence_ids": ["A1"]}]
    })");
    const json::Value report = verify_claims(draft, { "N1", "A1" }, "Банк России сохранил ставку 14% годовых");
    bool flagged = false, wrongly = false;
    for (const auto& issue : report.get("issues").as_array()) {
        if (issue.get("code").as_string() != "number_not_in_sources") continue;
        if (issue.get("text").as_string().find("23,5") != std::string::npos) flagged = true;
        if (issue.get("text").as_string().find("14") != std::string::npos) wrongly = true;
    }
    CHECK(flagged);
    CHECK(!wrongly);
}

TEST(verifier_accepts_clean_draft) {
    const json::Value draft = json::Value::parse(R"({
      "why_it_matters": {"text": "Дорогой долг", "evidence_ids": ["N1","C1"]},
      "risks": [{"text": "Рост расходов", "evidence_ids": ["N1"]}],
      "opportunities": [],
      "actions": [{"text": "Проверить график платежей", "evidence_ids": ["C1"]}],
      "bank_view": {"text": "Банк ждёт сохранения ставки", "evidence_ids": ["A2"]}
    })");
    const json::Value report = verify_claims(draft, { "N1", "C1", "A2" }, "");
    CHECK(report.get("ok").as_bool());
}

int main(int argc, char** argv) { return run_all_tests(argc > 1 ? argv[1] : ""); }
