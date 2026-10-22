#pragma once

#include "agent.hpp"
#include "analytics.hpp"
#include "data_store.hpp"
#include "news.hpp"

#include <string>

struct ServerConfig {
    std::string host = "127.0.0.1";
    unsigned short port = 8080;
    std::string api_token;
};

// Everything a request handler may use. Owned by main(); lives for the whole process.
struct AppServices {
    const DataStore& data;
    const AnalyticsIndex& analytics;
    InsightAgent& agent;
    NewsStore& news;
    NewsCollector& collector;
};

int run_http_server(const ServerConfig& config, AppServices& app);

