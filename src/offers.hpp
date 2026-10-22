#pragma once
// Native, clearly labelled offers of Alfa-Bank products tied to a concrete gap in the client's
// product set and to the event topic. The case allows native advertising in the news summary.
#include "data_store.hpp"
#include "json.hpp"

#include <string>
#include <vector>

// Result: up to two [{product_id, product, text, reason, label:"Предложение Альфа-Банка", is_advertising:true}]
json::Value bank_offers(const CompanyProfile& company, const std::vector<std::string>& categories);
