#include "feed.hpp"

#include "text_util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

struct TopicGroup {
    const char* name;
    std::vector<const char*> industry;
    std::vector<const char*> news;
};

// Industry topics: how a company's main activity (ОКВЭД name) and a news text refer to the same market.
const std::vector<TopicGroup>& topic_groups() {
    static const std::vector<TopicGroup> groups = {
        { "уголь", { "угл", "уголь" }, { "угл", "угол", "уголь" } },
        { "нефть и газ", { "нефт", "газ" }, { "нефт", "газ", "brent", "urals", "демпфер" } },
        { "металлургия", { "металл", "стал", "прокат", "труб", "чугун", "литей" }, { "стал", "металл", "прокат", "никел", "алюмин", "медь", "северстал" } },
        { "строительство и недвижимость", { "строител", "недвижим", "зданий", "девелоп", "сооружен" }, { "строител", "новострой", "ипотек", "застройщ", "жиль", "цемент" } },
        { "сельское хозяйство", { "зерн", "сельск", "выращиван", "растениевод", "животновод", "пищев", "мук" }, { "зерн", "пшениц", "урожа", "сельхоз", "ячмен", "кукуруз", "подсолнеч" } },
        { "транспорт и логистика", { "транспорт", "перевоз", "логист", "склад", "железнодор", "морск", "трубопровод" }, { "ржд", "погрузк", "логист", "перевоз", "фрахт", "порт", "танкер", "судох", "пролив" } },
        { "торговля", { "торговл" }, { "розниц", "продаж", "потреблен", "спрос", "маркетплейс", "интернет-магазин" } },
        { "автомобили", { "автомоб", "транспортных средств", "автотранспорт" }, { "автоваз", "lada", "автомоб", "авторын" } },
        { "энергетика", { "электроэнерг", "энерги", "тепл", "пара" }, { "электроэнерг", "тариф", "энерг" } },
        { "ИТ и связь", { "программ", "информац", "связ", "компьютер", "вычислит" }, { "ит-", "цифров", "нейросет", "искусствен", "ит-компан" } },
        { "химия и удобрения", { "хими", "удобрен" }, { "удобрен", "хими", "аммиак" } },
        { "финансы", { "финанс", "страхов", "кредитн", "банк" }, { "банк", "кредит", "ставк" } },
        { "драгоценные металлы", { "золот", "драгоцен", "благородн" }, { "золот", "серебр" } },
    };
    return groups;
}

std::vector<const TopicGroup*> company_topics(const std::string& industry) {
    std::vector<const TopicGroup*> out;
    if (industry.empty()) return out;
    const textutil::WordIndex words(industry);
    for (const auto& g : topic_groups()) if (words.any(g.industry)) out.push_back(&g);
    return out;
}

const TopicGroup* news_topic_for(const std::vector<const TopicGroup*>& topics, const textutil::WordIndex& news) {
    for (const auto* g : topics) if (news.any(g->news)) return g;
    return nullptr;
}

bool has(const std::vector<std::string>& v, const char* key) { return std::find(v.begin(), v.end(), key) != v.end(); }

// Category-level sensitivity when the text does not name the company's market directly.
bool category_sensitive(const textutil::WordIndex& industry, const std::vector<std::string>& c) {
    const auto contains_any = [&](const textutil::WordIndex& w, std::initializer_list<const char*> n) { return w.any(n); };
    const auto& lowered_industry = industry;
    if (has(c, "commodity") && contains_any(lowered_industry, { "металл", "стал", "угл", "нефт", "газ", "руд", "хим", "топлив", "зерн", "сельск", "добыч" })) return true;
    if (has(c, "logistics") && contains_any(lowered_industry, { "транспорт", "перевоз", "логист", "склад", "торговл", "добыч" })) return true;
    if (has(c, "rate") && contains_any(lowered_industry, { "строител", "недвижим", "лизинг", "оптов", "производств", "финанс" })) return true;
    if (has(c, "currency") && contains_any(lowered_industry, { "торговл", "производств", "транспорт", "добыч" })) return true;
    if (has(c, "sanctions") && contains_any(lowered_industry, { "торговл", "металл", "производств", "добыч", "транспорт" })) return true;
    if (has(c, "demand") && contains_any(lowered_industry, { "розничн", "торговл", "пищев", "производств", "строител", "автомоб" })) return true;
    if (has(c, "regulation") && contains_any(lowered_industry, { "торговл", "финанс", "энерг", "информац", "строител" })) return true;
    if (has(c, "technology") && contains_any(lowered_industry, { "информац", "связ", "программ" })) return true;
    return false;
}

double freshness(long long epoch, long long now) {
    if (epoch <= 0) return 0.5;
    const double age_days = std::max(0.0, static_cast<double>(now - epoch) / 86400.0);
    return 1.0 / (1.0 + age_days / 10.0);
}

std::string bank_document_title(const std::string& file) {
    static const std::pair<const char*, const char*> titles[] = {
        { "Inflation_tracker", "Инфляция: основные выводы" },
        { "Бонд_Бриф", "Бонд Бриф: итоги недели" },
        { "Gold_digest", "Обзор рынка золота, август 2026" },
        { "FI_Strategy_3Q26", "Тактический взгляд на 3-й квартал 2026 года" },
        { "Russian_Steels", "Commodities performance: металлурги" },
        { "Policy_Rate_11Sep", "Ключевая ставка ЦБ РФ: ставка без изменения на 14,0%" },
        { "ALFA-7M26", "Макростатистика 7М26: рынок труда" },
        { "ALFA-consumption", "Потребление на пике" },
        { "macro_stat1H26", "Макростатистика 1П26" },
    };
    for (const auto& t : titles) if (file.find(t.first) != std::string::npos) return t.second;
    return file;
}

} // namespace

const std::vector<RoleInfo>& roles() {
    static const std::vector<RoleInfo> list = {
        { "owner", "Собственник бизнеса", "стоимость бизнеса, долгосрочные риски, рынок и регулирование",
            { { "macro", 1.3 }, { "regulation", 1.25 }, { "demand", 1.2 }, { "sanctions", 1.2 } } },
        { "ceo", "Генеральный директор", "выручка, спрос, конкуренты, цепочки поставок и сырьё",
            { { "demand", 1.3 }, { "commodity", 1.25 }, { "logistics", 1.25 }, { "sanctions", 1.2 } } },
        { "cfo", "Финансовый директор", "стоимость долга, ставка, налоги, маржа и денежный поток",
            { { "rate", 1.35 }, { "regulation", 1.25 }, { "currency", 1.2 }, { "macro", 1.15 } } },
        { "treasurer", "Руководитель казначейства", "ликвидность, валютные позиции, расчёты, размещение свободных средств",
            { { "currency", 1.45 }, { "rate", 1.3 }, { "sanctions", 1.2 } } },
        { "accountant", "Главный бухгалтер", "налоги, отчётность, требования регулятора, расчёты с контрагентами",
            { { "regulation", 1.5 }, { "labor", 1.2 }, { "currency", 1.1 } } },
    };
    return list;
}

const RoleInfo* find_role(const std::string& id) {
    for (const auto& r : roles()) if (r.id == id) return &r;
    return nullptr;
}

const RoleInfo& role_or_default(const std::string& id) {
    const RoleInfo* r = find_role(id);
    return r ? *r : *find_role("ceo");
}

json::Value roles_json() {
    json::Value list = json::Value::array();
    for (const auto& r : roles()) {
        json::Value v = json::Value::object();
        v["id"] = r.id;
        v["title"] = r.title;
        v["focus"] = r.focus;
        json::Value priorities = json::Value::array();
        for (const auto& w : r.weights) priorities.push_back(news_category_label(w.first));
        v["priority_topics"] = std::move(priorities);
        list.push_back(std::move(v));
    }
    return list;
}

std::vector<std::string> profile_gaps(const CompanyProfile& c) {
    std::vector<std::string> gaps;
    if (c.industry.empty() && c.okved_code.empty()) gaps.push_back("Отрасль компании не указана: отраслевая привязка новостей недоступна");
    if (c.region.empty()) gaps.push_back("Регион компании не указан");
    if (c.revenue <= 0) gaps.push_back("Выручка компании не указана");
    if (c.counterparties.empty()) gaps.push_back("Нет данных о платежах с контрагентами");
    return gaps;
}

json::Value score_relevance(const CompanyProfile& company, const std::string& title, const std::string& text,
    const std::vector<std::string>& categories, const std::vector<std::string>& counterparty_industries,
    const RoleInfo& role) {
    const textutil::WordIndex news(title + " " + text);
    const textutil::WordIndex industry(company.industry);
    double score = 0.10;
    json::Value reasons = json::Value::array();

    const auto topics = company_topics(company.industry);
    const TopicGroup* direct = news_topic_for(topics, news);
    const bool sensitive = !direct && !company.industry.empty() && category_sensitive(industry, categories);
    if (direct) { score += 0.30; reasons.push_back(std::string("Новость напрямую касается отрасли компании: ") + direct->name); }
    else if (sensitive) { score += 0.15; reasons.push_back("Тема события чувствительна для отрасли компании"); }

    const bool rate = has(categories, "rate");
    if (rate && company.loans) { score += 0.22; reasons.push_back("У компании есть кредитные продукты, а событие влияет на стоимость заёмных денег"); }
    if (rate && (company.leasing || company.guarantees)) { score += 0.06; reasons.push_back("В профиле есть лизинг или банковские гарантии, их стоимость зависит от ставок"); }
    if (rate && company.deposits) { score += 0.04; reasons.push_back("У компании есть депозиты: доходность размещения зависит от ставки"); }
    if ((has(categories, "currency") || has(categories, "sanctions")) && company.fts) {
        score += 0.18; reasons.push_back("Компания ведёт внешнеторговую деятельность (есть данные ФТС), а событие касается валюты или внешней торговли");
    }
    if ((has(categories, "logistics") || has(categories, "commodity")) && (company.payments_in > 0 || company.payments_out > 0)) {
        score += 0.05; reasons.push_back("По платежам видна сеть поставщиков и покупателей, на которую может повлиять событие");
    }

    std::string counterparty_match;
    for (const auto& cp : counterparty_industries) {
        const TopicGroup* g = news_topic_for(company_topics(cp), news);
        if (g && g != direct) { counterparty_match = cp; break; }
    }
    if (!counterparty_match.empty()) { score += 0.12; reasons.push_back("Событие затрагивает отрасль ключевого контрагента по платежам: " + counterparty_match); }

    if (!company.region.empty() && news.lowered().find(textutil::lower(company.region)) != std::string::npos) {
        score += 0.10; reasons.push_back("В новости упомянут регион компании");
    }
    if (rate || has(categories, "macro")) { score += 0.05; reasons.push_back("Влияет на общий макроэкономический фон для крупного бизнеса"); }

    // Primary category: the topic that best explains why this event matters to this company.
    // Signals: category named in the title, a company feature it hits, the user's role priority.
    std::string primary = categories.empty() ? std::string("other") : categories.front();
    {
        const auto title_categories = classify_news(title);
        const bool fts = company.fts;
        const bool credit = company.loans || company.leasing || company.guarantees || company.deposits;
        double best = -1;
        for (const auto& c : categories) {
            double w = 0;
            if (std::find(title_categories.begin(), title_categories.end(), c) != title_categories.end()) w += 1.5;
            if (c == "rate" && credit) w += 1.0;
            if ((c == "currency" || c == "sanctions") && fts) w += 1.0;
            if ((c == "commodity" || c == "logistics" || c == "demand") && direct) w += 1.2;
            if (c == "macro" || c == "other") w -= 0.5;
            const auto rw = role.weights.find(c);
            if (rw != role.weights.end()) w += (rw->second - 1.0);
            if (w > best) { best = w; primary = c; }
        }
    }

    double role_factor = 1.0;
    std::string role_topic;
    for (const auto& c : categories) {
        const auto it = role.weights.find(c);
        if (it != role.weights.end() && it->second > role_factor) { role_factor = it->second; role_topic = c; }
    }
    if (!role_topic.empty())
        reasons.push_back("Для роли «" + role.title + "» приоритетна тема «" + news_category_label(role_topic) + "»");
    // Soft cap: many signals add up to high relevance without saturating at 100%,
    // so the feed still distinguishes strong from very strong matches.
    const double raw = score * role_factor;
    score = std::min(0.99, raw / (1.0 + 0.45 * raw));

    json::Value result = json::Value::object();
    result["score"] = std::round(score * 1000.0) / 1000.0;
    result["band"] = score >= 0.5 ? "high" : score >= 0.3 ? "medium" : "low";
    result["reasons"] = std::move(reasons);
    result["event_categories"] = json::Value::array();
    for (const auto& c : categories) result["event_categories"].push_back(news_category_label(c));
    result["industry_match"] = direct != nullptr || sensitive;
    result["direct_industry_topic"] = direct ? json::Value(direct->name) : json::Value(nullptr);
    result["counterparty_match"] = counterparty_match.empty() ? json::Value(nullptr) : json::Value(counterparty_match);
    result["role_boost"] = role_factor;
    result["primary_category"] = primary;
    result["primary_category_label"] = news_category_label(primary);
    result["interpretation"] = "Оценка по правилам для приоритизации ленты; не является вероятностью финансового эффекта";
    return result;
}

json::Value build_feed(const DataStore& data, const AnalyticsIndex& analytics, const NewsStore& news,
    const CompanyProfile& company, const std::string& role_id, size_t limit) {
    const RoleInfo& role = role_or_default(role_id);
    const auto cp_industries = data.counterparty_industries(company, 5);
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    struct Ranked { NewsItem item; json::Value relevance; double rank; };
    std::vector<Ranked> ranked;
    for (auto& item : news.recent(2000)) {
        json::Value rel = score_relevance(company, item.title, item.summary, item.categories, cp_industries, role);
        const double fresh = freshness(item.published_epoch, now);
        const double rank = rel.get("score").as_number() * (0.55 + 0.45 * fresh);
        ranked.push_back({ std::move(item), std::move(rel), rank });
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.rank > b.rank; });
    if (ranked.size() > limit) ranked.resize(limit);

    json::Value items = json::Value::array();
    for (auto& r : ranked) {
        json::Value v = news_to_json(r.item);
        v["news_id"] = r.item.id;
        v["feed_rank"] = std::round(r.rank * 1000.0) / 1000.0;
        const auto& reasons = r.relevance.get("reasons").as_array();
        std::string why;
        for (const auto& reason : reasons) {
            const std::string& t = reason.as_string();
            if (t.rfind("Влияет на общий", 0) == 0 || t.rfind("Для роли", 0) == 0) continue;
            if (!why.empty()) why += ". ";
            why += t;
            if (why.size() > 160) break;
        }
        if (why.empty()) why = "Общий фон для крупного бизнеса; прямой связи с профилем компании не найдено";
        v["why_for_you"] = why;
        v["relevance"] = std::move(r.relevance);
        const auto hits = analytics.search(r.item.title + " " + r.item.summary, 1);
        if (!hits.empty() && hits.front().score > 1.0) {
            json::Value hint = json::Value::object();
            hint["document"] = hits.front().document;
            hint["title"] = bank_document_title(hits.front().document);
            hint["page"] = hits.front().page;
            hint["label"] = "Есть материал Альфа-Банка по теме";
            v["bank_hint"] = std::move(hint);
        } else {
            v["bank_hint"] = nullptr;
        }
        items.push_back(std::move(v));
    }

    json::Value result = json::Value::object();
    result["company_id"] = company.id;
    json::Value role_json = json::Value::object();
    role_json["id"] = role.id;
    role_json["title"] = role.title;
    role_json["focus"] = role.focus;
    result["role"] = std::move(role_json);
    result["counterparty_industries"] = json::Value::array();
    for (const auto& c : cp_industries) result["counterparty_industries"].push_back(c);
    result["data_gaps"] = json::Value::array();
    for (const auto& g : profile_gaps(company)) result["data_gaps"].push_back(g);
    result["total_news"] = static_cast<long long>(news.size());
    result["items"] = std::move(items);
    result["method"] = "rule_based_ranking";
    return result;
}
