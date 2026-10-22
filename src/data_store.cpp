#include "data_store.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace {

class DelimitedReader {
public:
    explicit DelimitedReader(std::istream& input) : input_(input) {}

    bool next(std::vector<std::string>& row) {
        row.clear();
        std::string cell;
        bool quoted = false;
        bool saw_anything = false;
        char c = 0;
        while (input_.get(c)) {
            saw_anything = true;
            if (quoted) {
                if (c == '"') {
                    if (input_.peek() == '"') { input_.get(c); cell.push_back('"'); }
                    else quoted = false;
                } else {
                    cell.push_back(c);
                }
                continue;
            }
            if (c == '"' && cell.empty()) { quoted = true; continue; }
            if (c == ';') { row.push_back(std::move(cell)); cell.clear(); continue; }
            if (c == '\n') { row.push_back(std::move(cell)); return true; }
            if (c == '\r') {
                if (input_.peek() == '\n') input_.get(c);
                row.push_back(std::move(cell));
                return true;
            }
            cell.push_back(c);
        }
        if (saw_anything || !cell.empty() || !row.empty()) {
            row.push_back(std::move(cell));
            return true;
        }
        return false;
    }

private:
    std::istream& input_;
};

std::string trim_ascii(std::string value) {
    auto blank = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    auto first = std::find_if_not(value.begin(), value.end(), blank);
    auto last = std::find_if_not(value.rbegin(), value.rend(), blank).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string field(const std::vector<std::string>& row, const std::unordered_map<std::string, size_t>& columns,
    const std::string& name) {
    auto it = columns.find(name);
    if (it == columns.end() || it->second >= row.size()) return {};
    return trim_ascii(row[it->second]);
}

double number(const std::string& value) {
    if (value.empty()) return 0;
    try {
        size_t used = 0;
        double result = std::stod(value, &used);
        return used == value.size() && std::isfinite(result) ? result : 0;
    } catch (...) { return 0; }
}

long long integer(const std::string& value) {
    const double parsed = number(value);
    if (parsed <= 0) return 0;
    return static_cast<long long>(std::min(parsed, static_cast<double>(std::numeric_limits<long long>::max())));
}

bool flag(const std::string& value) {
    const std::string v = lower_ascii(value);
    return v == "1" || v == "1.0" || v == "true" || v == "yes" || v == "да";
}

std::string industry_key(const std::string& code) {
    std::string digits;
    for (char c : code) {
        if (c >= '0' && c <= '9') digits.push_back(c);
        else if (!digits.empty()) break;
    }
    if (digits.size() > 2) digits.resize(2);
    return digits;
}

std::string classify_industry(const std::string& code, const std::string& name) {
    const std::string key = industry_key(code);
    int section = -1;
    try { if (!key.empty()) section = std::stoi(key); } catch (...) {}
    if (section >= 1 && section <= 3) return "Сельское хозяйство и добыча биоресурсов";
    if (section >= 5 && section <= 9) return "Добыча полезных ископаемых";
    if (section >= 10 && section <= 33) return "Обрабатывающая промышленность";
    if (section == 35) return "Энергетика и коммунальная инфраструктура";
    if (section >= 36 && section <= 39) return "Водоснабжение и обращение с отходами";
    if (section >= 41 && section <= 43) return "Строительство";
    if (section >= 45 && section <= 47) return "Оптовая и розничная торговля";
    if (section >= 49 && section <= 53) return "Транспорт и логистика";
    if (section >= 55 && section <= 56) return "Гостиничный и ресторанный бизнес";
    if (section >= 58 && section <= 63) return "Информация и связь";
    if (section >= 64 && section <= 66) return "Финансовая деятельность";
    if (section == 68) return "Операции с недвижимостью";
    if (section >= 69 && section <= 75) return "Профессиональная и научная деятельность";
    if (section >= 77 && section <= 82) return "Административная деятельность";
    if (section == 85) return "Образование";
    if (section >= 86 && section <= 88) return "Здравоохранение и социальная сфера";
    if (section >= 90 && section <= 93) return "Культура, спорт и досуг";
    if (section >= 94 && section <= 96) return "Прочие услуги";
    if (!name.empty()) return name;
    return "Отрасль не определена";
}

uint32_t uid_number(const std::string& uid) {
    size_t pos = uid.find_first_of("0123456789");
    if (pos == std::string::npos) return 0;
    uint64_t value = 0;
    for (; pos < uid.size() && uid[pos] >= '0' && uid[pos] <= '9'; ++pos) {
        value = value * 10 + static_cast<unsigned>(uid[pos] - '0');
        if (value > std::numeric_limits<uint32_t>::max()) return 0;
    }
    return static_cast<uint32_t>(value);
}

std::string uid_string(uint32_t id) { return "U_" + std::to_string(id); }

std::string dataset_path(const std::string& data_dir, const std::string& file) {
    if (data_dir.empty()) return file;
    const char last = data_dir.back();
    return data_dir + ((last == '/' || last == '\\') ? "" : "/") + file;
}

struct PaymentAggregate { double amount = 0; long long count = 0; };
struct NumericPartner { uint32_t id = 0; bool outgoing = false; double amount = 0; long long count = 0; };

} // namespace

bool DataStore::load(const std::string& data_dir, std::string& error) {
    companies_.clear();
    by_id_.clear();
    by_group_.clear();
    payment_edge_count_ = 0;
    relationship_count_ = 0;

    std::ifstream input(dataset_path(data_dir, "df_Uid_data_hack.csv"), std::ios::binary);
    if (!input) { error = "Cannot open data/df_Uid_data_hack.csv"; return false; }
    DelimitedReader reader(input);
    std::vector<std::string> header;
    if (!reader.next(header)) { error = "Company dataset has no header"; return false; }
    if (!header.empty() && header[0].size() >= 3 &&
        static_cast<unsigned char>(header[0][0]) == 0xEF &&
        static_cast<unsigned char>(header[0][1]) == 0xBB &&
        static_cast<unsigned char>(header[0][2]) == 0xBF) header[0].erase(0, 3);
    std::unordered_map<std::string, size_t> columns;
    for (size_t i = 0; i < header.size(); ++i) columns[trim_ascii(header[i])] = i;

    std::vector<std::string> row;
    while (reader.next(row)) {
        CompanyProfile item;
        item.id = field(row, columns, "SYNTHETIC_U");
        if (item.id.empty()) continue;
        item.organization_type = field(row, columns, "ORG_TYPE");
        item.ownership = field(row, columns, "OKFS_NAME");
        item.age_band = field(row, columns, "COMPANY_AGE_YEARS_GRADATION");
        item.development_stage = field(row, columns, "ECO_MARK_LABEL");
        item.okved_code = field(row, columns, "MAINOKVED_CODE");
        item.industry = field(row, columns, "MAINOKVED_NAME");
        item.client_type = field(row, columns, "CL_TYPE");
        item.fns_workers = field(row, columns, "AMOUNT_WORKERS_FNS_GRADATION");
        item.spark_workers = field(row, columns, "AMOUNT_WORKERS_SPARK_GRADATION");
        item.city = field(row, columns, "CITY");
        item.region = field(row, columns, "REGION");
        item.fts_country = field(row, columns, "FTS_COUNTRY_NAME");
        item.group_id = field(row, columns, "GROUP_ID");
        item.revenue = number(field(row, columns, "REVENUE_COMPANY"));
        item.negative_mark = flag(field(row, columns, "NEGATIVEMARK"));
        item.rko = flag(field(row, columns, "CL_PRODUCT_RKO_MARK"));
        item.guarantees = flag(field(row, columns, "CL_PRODUCT_GAR_MARK"));
        item.loans = flag(field(row, columns, "CL_PRODUCT_LOA_MARK"));
        item.leasing = flag(field(row, columns, "CL_PRODUCT_LEA_MARK"));
        item.deposits = flag(field(row, columns, "CL_PRODUCT_DEP_MARK"));
        item.salary_project = flag(field(row, columns, "CL_PRODUCT_ZPP_MARK"));
        item.fts = flag(field(row, columns, "FTS_FLAG"));
        item.factoring = flag(field(row, columns, "FACTORING_MARK"));
        const size_t index = companies_.size();
        by_id_[item.id] = index;
        if (!item.group_id.empty()) by_group_[item.group_id].push_back(index);
        companies_.push_back(std::move(item));
    }

    if (companies_.empty()) { error = "Company dataset contains no records"; return false; }
    for (const auto& group : by_group_) {
        for (size_t index : group.second) {
            companies_[index].group_size = static_cast<int>(group.second.size());
            companies_[index].related_companies = static_cast<int>(group.second.size() - (group.second.size() > 0 ? 1 : 0));
        }
    }
    calculate_industry_positions();
    apply_relationships(dataset_path(data_dir, "df_Fid_Uid_link_hack.csv"));
    apply_payments(dataset_path(data_dir, "df_Uid_payment_hack.csv"));
    return true;
}

void DataStore::calculate_industry_positions() {
    std::unordered_map<std::string, std::vector<double>> by_industry;
    for (const auto& company : companies_) {
        const std::string key = industry_key(company.okved_code);
        if (!key.empty() && company.revenue > 0) by_industry[key].push_back(company.revenue);
    }
    for (auto& item : by_industry) std::sort(item.second.begin(), item.second.end());
    for (auto& company : companies_) {
        const std::string key = industry_key(company.okved_code);
        const auto it = by_industry.find(key);
        if (it == by_industry.end() || it->second.empty() || company.revenue <= 0) continue;
        const auto pos = std::upper_bound(it->second.begin(), it->second.end(), company.revenue);
        company.industry_percentile = 100.0 * static_cast<double>(pos - it->second.begin()) / it->second.size();
    }
}

void DataStore::apply_relationships(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    DelimitedReader reader(input);
    std::vector<std::string> header, row;
    if (!reader.next(header)) return;
    std::unordered_map<std::string, size_t> columns;
    for (size_t i = 0; i < header.size(); ++i) columns[trim_ascii(header[i])] = i;
    std::unordered_map<std::string, std::vector<size_t>> by_person;
    std::unordered_map<size_t, std::unordered_set<std::string>> people_by_company;
    while (reader.next(row)) {
        const std::string person = field(row, columns, "SYNTHETIC_F");
        const std::string company = field(row, columns, "SYNTHETIC_U");
        auto it = by_id_.find(company);
        if (person.empty() || it == by_id_.end()) continue;
        by_person[person].push_back(it->second);
        people_by_company[it->second].insert(person);
    }
    std::vector<std::unordered_set<size_t>> related_by_company(companies_.size());
    for (const auto& group : by_group_) {
        for (size_t index : group.second) {
            for (size_t other : group.second) if (other != index) related_by_company[index].insert(other);
        }
    }
    for (auto& item : by_person) {
        auto& indices = item.second;
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        if (indices.empty()) continue;
        ++relationship_count_;
        if (indices.size() < 2) continue;
        for (size_t index : indices) {
            for (size_t other : indices) if (other != index) related_by_company[index].insert(other);
        }
    }
    for (const auto& item : people_by_company) {
        companies_[item.first].linked_people = static_cast<int>(item.second.size());
    }
    for (size_t index = 0; index < companies_.size(); ++index)
        companies_[index].related_companies = static_cast<int>(related_by_company[index].size());
}

void DataStore::apply_payments(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    DelimitedReader reader(input);
    std::vector<std::string> header, row;
    if (!reader.next(header)) return;
    std::unordered_map<std::string, size_t> columns;
    for (size_t i = 0; i < header.size(); ++i) columns[trim_ascii(header[i])] = i;
    std::unordered_map<uint64_t, PaymentAggregate> edges;
    edges.reserve(1'600'000);
    while (reader.next(row)) {
        const uint32_t beneficiary = uid_number(field(row, columns, "BENEFICIARY"));
        const uint32_t payer = uid_number(field(row, columns, "PAYER"));
        if (!beneficiary || !payer) continue;
        const double amount = number(field(row, columns, "PAY_AMT"));
        const long long count = integer(field(row, columns, "PAY_CNT"));
        const uint64_t key = (static_cast<uint64_t>(payer) << 32) | beneficiary;
        auto& aggregate = edges[key];
        aggregate.amount += amount;
        aggregate.count += count;
    }

    std::unordered_map<uint32_t, std::vector<NumericPartner>> adjacency;
    adjacency.reserve(100'000);
    for (const auto& edge : edges) {
        const uint32_t payer = static_cast<uint32_t>(edge.first >> 32);
        const uint32_t beneficiary = static_cast<uint32_t>(edge.first & 0xFFFFFFFFu);
        const auto& amount = edge.second;
        adjacency[payer].push_back({ beneficiary, true, amount.amount, amount.count });
        adjacency[beneficiary].push_back({ payer, false, amount.amount, amount.count });
        auto payer_it = by_id_.find(uid_string(payer));
        if (payer_it != by_id_.end()) {
            auto& company = companies_[payer_it->second];
            company.payments_out += amount.amount;
            company.payment_count_out += amount.count;
        }
        auto beneficiary_it = by_id_.find(uid_string(beneficiary));
        if (beneficiary_it != by_id_.end()) {
            auto& company = companies_[beneficiary_it->second];
            company.payments_in += amount.amount;
            company.payment_count_in += amount.count;
        }
    }
    payment_edge_count_ = edges.size();
    for (auto& item : adjacency) {
        auto company_it = by_id_.find(uid_string(item.first));
        if (company_it == by_id_.end()) continue;
        auto& partners = item.second;
        std::sort(partners.begin(), partners.end(), [](const NumericPartner& a, const NumericPartner& b) {
            if (a.amount != b.amount) return a.amount > b.amount;
            return a.count > b.count;
        });
        auto& profile = companies_[company_it->second];
        const size_t limit = std::min<size_t>(8, partners.size());
        profile.counterparties.reserve(limit);
        for (size_t i = 0; i < limit; ++i) {
            const auto& partner = partners[i];
            const std::string partner_id = uid_string(partner.id);
            profile.counterparties.push_back({ partner_id, partner.outgoing ? "outgoing" : "incoming",
                partner.amount, partner.count, by_id_.find(partner_id) != by_id_.end() });
        }
    }
}

const CompanyProfile* DataStore::find(const std::string& id) const {
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &companies_[it->second];
}

json::Value DataStore::profile_json(const CompanyProfile& p) const {
    json::Value result = json::Value::object();
    result["id"] = p.id;
    result["client_type"] = p.client_type.empty() ? json::Value(nullptr) : json::Value(p.client_type);
    result["organization_type"] = p.organization_type.empty() ? json::Value(nullptr) : json::Value(p.organization_type);
    result["ownership"] = p.ownership.empty() ? json::Value(nullptr) : json::Value(p.ownership);
    result["company_age_band"] = p.age_band.empty() ? json::Value(nullptr) : json::Value(p.age_band);
    result["development_stage"] = p.development_stage.empty() ? json::Value(nullptr) : json::Value(p.development_stage);
    result["industry"] = json::Value::object();
    result["industry"]["okved_code"] = p.okved_code.empty() ? json::Value(nullptr) : json::Value(p.okved_code);
    result["industry"]["name"] = p.industry.empty() ? json::Value(nullptr) : json::Value(p.industry);
    result["industry"]["category"] = classify_industry(p.okved_code, p.industry);
    result["financials"] = json::Value::object();
    result["financials"]["annual_revenue_rub"] = p.revenue > 0 ? json::Value(p.revenue) : json::Value(nullptr);
    result["financials"]["case_large_business_threshold_rub"] = 15000000000LL;
    result["financials"]["meets_case_large_business_threshold"] = p.revenue >= 15000000000.0;
    result["financials"]["revenue_percentile_in_dataset_industry"] = p.industry_percentile > 0 ? json::Value(p.industry_percentile) : json::Value(nullptr);
    result["financials"]["market_share"] = nullptr;
    result["financials"]["market_share_note"] = "Not present in supplied data; dataset percentile is not market share";
    result["employees"] = json::Value::object();
    result["employees"]["fns_band"] = p.fns_workers.empty() ? json::Value(nullptr) : json::Value(p.fns_workers);
    result["employees"]["spark_band"] = p.spark_workers.empty() ? json::Value(nullptr) : json::Value(p.spark_workers);
    result["location"] = json::Value::object();
    result["location"]["city"] = p.city.empty() ? json::Value(nullptr) : json::Value(p.city);
    result["location"]["region"] = p.region.empty() ? json::Value(nullptr) : json::Value(p.region);
    result["products"] = json::Value::object();
    result["products"]["rko"] = p.rko;
    result["products"]["bank_guarantees"] = p.guarantees;
    result["products"]["loans"] = p.loans;
    result["products"]["leasing"] = p.leasing;
    result["products"]["deposits"] = p.deposits;
    result["products"]["salary_project"] = p.salary_project;
    result["products"]["factoring"] = p.factoring;
    result["trade"] = json::Value::object();
    result["trade"]["has_fts_data"] = p.fts;
    result["trade"]["fts_country"] = p.fts_country.empty() ? json::Value(nullptr) : json::Value(p.fts_country);
    result["network"] = json::Value::object();
    result["network"]["group_id"] = p.group_id.empty() ? json::Value(nullptr) : json::Value(p.group_id);
    result["network"]["group_member_count"] = p.group_size;
    result["network"]["related_company_count"] = p.related_companies;
    result["network"]["linked_person_record_count"] = p.linked_people;
    result["payments"] = json::Value::object();
    result["payments"]["incoming_amount_rub"] = p.payments_in;
    result["payments"]["outgoing_amount_rub"] = p.payments_out;
    result["payments"]["incoming_transaction_count"] = p.payment_count_in;
    result["payments"]["outgoing_transaction_count"] = p.payment_count_out;
    result["data_quality"] = json::Value::object();
    result["data_quality"]["revenue_available"] = p.revenue > 0;
    result["data_quality"]["industry_available"] = !p.okved_code.empty() || !p.industry.empty();
    result["data_quality"]["location_available"] = !p.region.empty() || !p.city.empty();
    result["data_quality"]["employee_data_available"] = !p.fns_workers.empty() || !p.spark_workers.empty();
    result["data_quality"]["warning"] = "Blank source fields are unknown, not zero or false";
    result["source"] = "df_Uid_data_hack.csv; df_Uid_payment_hack.csv; df_Fid_Uid_link_hack.csv";
    return result;
}

json::Value DataStore::network_json(const CompanyProfile& p) const {
    json::Value result = json::Value::object();
    result["company_id"] = p.id;
    result["group_id"] = p.group_id.empty() ? json::Value(nullptr) : json::Value(p.group_id);
    result["group_member_count"] = p.group_size;
    result["related_company_count"] = p.related_companies;
    result["linked_person_record_count"] = p.linked_people;
    result["payment_totals"] = json::Value::object();
    result["payment_totals"]["incoming_amount_rub"] = p.payments_in;
    result["payment_totals"]["outgoing_amount_rub"] = p.payments_out;
    result["payment_totals"]["incoming_transaction_count"] = p.payment_count_in;
    result["payment_totals"]["outgoing_transaction_count"] = p.payment_count_out;
    json::Value counterparties = json::Value::array();
    for (const auto& c : p.counterparties) {
        json::Value item = json::Value::object();
        item["company_id"] = c.company_id;
        item["direction"] = c.direction;
        item["amount_rub"] = c.amount;
        item["payment_count"] = c.payment_count;
        item["profile_available"] = c.profile_available;
        counterparties.push_back(std::move(item));
    }
    result["top_counterparties"] = std::move(counterparties);
    result["source"] = "df_Uid_payment_hack.csv and df_Fid_Uid_link_hack.csv; identifiers are synthetic";
    return result;
}

std::vector<const CompanyProfile*> DataStore::search(const std::string& query, size_t limit) const {
    std::vector<const CompanyProfile*> result;
    const std::string needle = lower_ascii(query);
    if (needle.empty()) return result;
    for (const auto& company : companies_) {
        const std::string id = lower_ascii(company.id);
        const std::string code = lower_ascii(company.okved_code);
        if (id == needle || code == needle) result.push_back(&company);
        if (result.size() >= limit) break;
    }
    return result;
}
