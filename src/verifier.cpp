#include "text_util.hpp"
#include "verifier.hpp"

#include <cctype>
#include <vector>

namespace {

std::string text_of(const json::Value& v) {
    if (v.is_string()) return v.as_string();
    if (v.get("text").is_string()) return v.get("text").as_string();
    return {};
}

bool cites_valid(const json::Value& claim, const std::unordered_set<std::string>& ids, bool require_bank = false) {
    for (const auto& id : claim.get("evidence_ids").as_array()) {
        if (!id.is_string() || !ids.count(id.as_string())) continue;
        if (!require_bank || id.as_string().rfind("A", 0) == 0) return true;
    }
    return false;
}

// Numbers like 14, 14%, 23,5, 1.8 with at least two digits or a decimal part; normalized to dot.
std::vector<std::string> numbers_in(const std::string& text) {
    std::vector<std::string> out;
    for (size_t i = 0; i < text.size();) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; continue; }
        size_t j = i;
        std::string n;
        while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) ||
               ((text[j] == ',' || text[j] == '.') && j + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[j + 1]))))) {
            n.push_back(text[j] == ',' ? '.' : text[j]);
            ++j;
        }
        if (n.size() >= 2 || n.find('.') != std::string::npos) out.push_back(n);
        i = j;
    }
    return out;
}

std::string strip_trailing_zero(std::string n) {
    if (n.find('.') == std::string::npos) return n;
    while (!n.empty() && n.back() == '0') n.pop_back();
    if (!n.empty() && n.back() == '.') n.pop_back();
    return n;
}

} // namespace

json::Value verify_claims(const json::Value& draft, const std::unordered_set<std::string>& valid_ids,
    const std::string& sources_text) {
    json::Value issues = json::Value::array();
    const auto add = [&](const char* code, const std::string& section, const std::string& text) {
        json::Value issue = json::Value::object();
        issue["code"] = code;
        issue["section"] = section;
        issue["text"] = textutil::truncate_utf8(text, 400);
        issues.push_back(std::move(issue));
    };

    std::string normalized_sources;
    for (char c : sources_text) normalized_sources.push_back(c == ',' ? '.' : c);
    std::vector<std::string> source_numbers;
    for (const auto& n : numbers_in(sources_text)) source_numbers.push_back(strip_trailing_zero(n));

    long long total = 0, supported = 0;
    for (const char* section : { "potential_effects", "risks", "opportunities", "actions", "model_conclusions" }) {
        for (const auto& claim : draft.get(section).as_array()) {
            const std::string text = text_of(claim);
            if (text.empty()) continue;
            ++total;
            if (cites_valid(claim, valid_ids)) ++supported;
            else add("no_evidence", section, text);
            for (const auto& n : numbers_in(text)) {
                const std::string key = strip_trailing_zero(n);
                bool found = false;
                for (const auto& s : source_numbers) if (s == key) { found = true; break; }
                if (!found) add("number_not_in_sources", section, "Число " + n + " не найдено в источниках: " + text);
            }
        }
    }
    const json::Value& bank = draft.get("bank_view");
    if (bank.is_object() && !text_of(bank).empty() && !cites_valid(bank, valid_ids, true))
        add("bank_view_without_bank_source", "bank_view", text_of(bank));
    const json::Value& why = draft.get("why_it_matters");
    if (text_of(why).empty()) add("empty_section", "why_it_matters", "Не объяснено, почему событие важно для компании");
    else {
        ++total;
        if (why.is_object() && cites_valid(why, valid_ids)) ++supported;
        else add("no_evidence", "why_it_matters", text_of(why));
    }
    if (draft.get("risks").as_array().empty() && draft.get("opportunities").as_array().empty())
        add("empty_section", "risks", "Нет ни рисков, ни возможностей");

    json::Value report = json::Value::object();
    report["ok"] = issues.as_array().empty();
    report["claims_total"] = total;
    report["claims_supported"] = supported;
    report["issues"] = std::move(issues);
    return report;
}
