// Backend unit tests. Run from the project root: backend_tests [filter]
#include "test_harness.hpp"

#include "data_store.hpp"

#include <cstdlib>
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

int main(int argc, char** argv) { return run_all_tests(argc > 1 ? argv[1] : ""); }
