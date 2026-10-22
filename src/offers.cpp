#include "offers.hpp"

#include <algorithm>

namespace {
bool has(const std::vector<std::string>& v, const char* key) { return std::find(v.begin(), v.end(), key) != v.end(); }

json::Value offer(const char* id, const char* product, const char* text, const std::string& reason) {
    json::Value o = json::Value::object();
    o["product_id"] = id;
    o["product"] = product;
    o["text"] = text;
    o["reason"] = reason;
    o["label"] = "Предложение Альфа-Банка";
    o["is_advertising"] = true;
    return o;
}
}

json::Value bank_offers(const CompanyProfile& c, const std::vector<std::string>& categories) {
    json::Value out = json::Value::array();
    const auto push = [&](json::Value o) { if (out.as_array().size() < 2) out.push_back(std::move(o)); };
    const bool fx_event = has(categories, "currency") || has(categories, "sanctions");
    if (fx_event && c.fts) {
        push(offer("fx_hedging", "Валютные форварды и свопы",
            "Зафиксируйте курс по будущим валютным платежам и поступлениям, чтобы колебания рубля не съедали маржу.",
            "У компании есть внешнеторговая деятельность, событие касается валюты или внешней торговли"));
        push(offer("ved_support", "Сопровождение ВЭД и валютный контроль",
            "Расчёты с зарубежными контрагентами в юанях и других валютах с сопровождением валютного контроля.",
            "Новость влияет на условия внешнеторговых расчётов"));
    }
    if (has(categories, "rate") && c.loans)
        push(offer("refinancing", "Рефинансирование и кредит с фиксированной ставкой",
            "Обсудите с менеджером перевод части долга на фиксированную ставку или рефинансирование, чтобы зафиксировать стоимость денег.",
            "У компании есть кредитные продукты, а событие влияет на стоимость заёмных средств"));
    if (has(categories, "rate") && !c.deposits && c.payments_in > 0)
        push(offer("deposit", "Депозит и неснижаемый остаток",
            "При высокой ставке свободная ликвидность приносит доход: разместите её на депозите или неснижаемом остатке.",
            "Через счета компании проходят поступления, а депозитов в профиле нет"));
    if ((has(categories, "logistics") || has(categories, "demand")) && !c.factoring && c.payments_in > 0)
        push(offer("factoring", "Факторинг",
            "Получайте деньги за отгрузку сразу, не дожидаясь оплаты покупателей, если сроки расчётов растягиваются.",
            "Событие может удлинить сроки оплаты покупателей, а факторинга в профиле нет"));
    if (has(categories, "regulation") && !c.guarantees)
        push(offer("guarantees", "Банковские гарантии",
            "Гарантии для контрактов и таможенных платежей без отвлечения денег из оборота.",
            "Изменение регулирования может потребовать обеспечения обязательств"));
    if (has(categories, "labor") && !c.salary_project)
        push(offer("salary_project", "Зарплатный проект",
            "Зарплатный проект с бонусами для сотрудников помогает удерживать персонал на конкурентном рынке труда.",
            "Событие касается рынка труда, а зарплатного проекта в профиле нет"));
    return out;
}
