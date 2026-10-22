#pragma once

#include "json.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct Counterparty {
    std::string company_id;
    std::string direction;
    double amount = 0;
    long long payment_count = 0;
    bool profile_available = false;
};

struct CompanyProfile {
    std::string id;
    std::string organization_type;
    std::string ownership;
    std::string age_band;
    std::string development_stage;
    std::string okved_code;
    std::string industry;
    std::string client_type;
    std::string fns_workers;
    std::string spark_workers;
    std::string city;
    std::string region;
    std::string fts_country;
    std::string group_id;
    double revenue = 0;
    bool negative_mark = false;
    bool rko = false;
    bool guarantees = false;
    bool loans = false;
    bool leasing = false;
    bool deposits = false;
    bool salary_project = false;
    bool fts = false;
    bool factoring = false;
    int group_size = 1;
    int linked_people = 0;
    int related_companies = 0;
    double industry_percentile = 0;
    double payments_in = 0;
    double payments_out = 0;
    long long payment_count_in = 0;
    long long payment_count_out = 0;
    std::vector<Counterparty> counterparties;
};

struct ClientFilter {
    std::string query;
    std::string industry;
    std::string region;
    double min_revenue = 15000000000.0;
    size_t limit = 50;
    size_t offset = 0;
};

class DataStore {
public:
    bool load(const std::string& data_dir, std::string& error);
    bool ready() const { return !companies_.empty(); }
    size_t company_count() const { return companies_.size(); }
    size_t payment_edge_count() const { return payment_edge_count_; }
    size_t relationship_count() const { return relationship_count_; }
    const CompanyProfile* find(const std::string& id) const;
    json::Value profile_json(const CompanyProfile& profile) const;
    json::Value network_json(const CompanyProfile& profile) const;
    std::vector<const CompanyProfile*> search(const std::string& query, size_t limit) const;
    json::Value list_clients(const ClientFilter& filter) const;
    json::Value client_facets(double min_revenue) const;
    json::Value client_summary(const CompanyProfile& profile) const;
    std::vector<std::string> counterparty_industries(const CompanyProfile& profile, size_t top) const;

private:
    std::vector<CompanyProfile> companies_;
    std::unordered_map<std::string, size_t> by_id_;
    std::unordered_map<std::string, std::vector<size_t>> by_group_;
    size_t payment_edge_count_ = 0;
    size_t relationship_count_ = 0;

    void apply_relationships(const std::string& path);
    void apply_payments(const std::string& path);
    void calculate_industry_positions();
};
