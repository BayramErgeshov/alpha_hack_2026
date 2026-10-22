#pragma once
// Programmatic checks of a model draft before it reaches the user:
// every claim must cite a known source, the bank position must cite Alfa-Bank material (A*),
// numbers in claims must appear in the sources, key sections must not be empty.
#include "json.hpp"

#include <string>
#include <unordered_set>

// Result: {ok, claims_total, claims_supported, issues:[{code, section, text}]}
// codes: no_evidence, bank_view_without_bank_source, number_not_in_sources, empty_section
json::Value verify_claims(const json::Value& draft, const std::unordered_set<std::string>& valid_ids,
    const std::string& sources_text);
