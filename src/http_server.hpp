#pragma once

#include "agent.hpp"
#include "analytics.hpp"
#include "data_store.hpp"

#include <string>

struct ServerConfig {
    std::string host = "127.0.0.1";
    unsigned short port = 8080;
    std::string api_token;
};

int run_http_server(const ServerConfig& config, const DataStore& data,
    const AnalyticsIndex& analytics, InsightAgent& agent);

