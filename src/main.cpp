#define _CRT_SECURE_NO_WARNINGS
#include "agent.hpp"
#include "analytics.hpp"
#include "data_store.hpp"
#include "http_server.hpp"
#include "llm_client.hpp"
#include "news.hpp"
#include "budget.hpp"
#include "metrics.hpp"

#include <filesystem>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <curl/curl.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace {
std::string env(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

unsigned short port_from_env() {
    const std::string value = env("ALFA_PORT", "8080");
    try { return static_cast<unsigned short>(std::clamp(std::stoi(value), 1, 65535)); }
    catch (...) { return 8080; }
}
}

int main() {
#ifdef _WIN32
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::cerr << "Could not initialize Winsock\n";
        return 1;
    }
#endif
    const CURLcode curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_status != CURLE_OK) {
        std::cerr << "Could not initialize libcurl\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    DataStore data;
    std::string error;
    const std::string data_dir = env("ALFA_DATA_DIR", "data/input");
    if (!data.load(data_dir, error)) {
        std::cerr << "Dataset load failed: " << error << '\n';
        curl_global_cleanup();
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }
    AnalyticsIndex analytics;
    const std::string analytics_dir = env("ALFA_ANALYTICS_DIR", "data/analytics");
    if (!analytics.load(analytics_dir, error)) {
        std::cerr << "Analytics load failed: " << error << '\n';
        curl_global_cleanup();
#ifdef _WIN32
        WSACleanup();
#endif
        return 3;
    }

    const LlmConfig llm = LlmConfig::from_environment();
    if (!llm.configured()) std::cerr << "LLM is not configured; deterministic analysis fallback is active\n";
    if (llm.configured() && !allowed_llm_base_url(llm.base_url))
        std::cerr << "LLM endpoint is outside the case allowlist; LLM requests will be rejected\n";
#if defined(__GLIBC__)
    malloc_trim(0); // CSV loading leaves large freed blocks; return them to the OS (matters on 512 MB hosts)
#endif
    NewsStore news;
    const std::string news_seed = env("ALFA_NEWS_SEED", "data/news/seed.json");
    if (!news.load_seed(news_seed, error)) std::cerr << "News seed not loaded: " << error << '\n';
    const std::string work_dir = env("ALFA_WORK_DIR", "work");
    std::error_code fs_error;
    std::filesystem::create_directories(work_dir, fs_error);
    const std::string news_state = (std::filesystem::path(work_dir) / "news.json").string();
    news.load_state(news_state);
    NewsCollector collector(news, news_state);
    int refresh_minutes = 30;
    try { refresh_minutes = std::clamp(std::stoi(env("ALFA_NEWS_REFRESH_MINUTES", "30")), 0, 24 * 60); } catch (...) {}
    collector.start(refresh_minutes);
    long long token_limit = 400000;
    try { token_limit = std::max(1LL, std::stoll(env("ALFA_LLM_DAILY_TOKEN_LIMIT", "400000"))); } catch (...) {}
    TokenBudget budget(token_limit, (std::filesystem::path(work_dir) / "budget.json").string());
    InsightAgent agent(data, analytics, llm, budget);
    agent.attach_news(&news);
    ServerConfig server;
    server.host = env("ALFA_HOST", "127.0.0.1");
    server.port = port_from_env();
    server.api_token = env("ALFA_API_TOKEN", "");
    server.static_dir = env("ALFA_STATIC_DIR", "");
    server.cors_origin = env("ALFA_CORS_ORIGIN", "");
    std::cout << "Loaded " << data.company_count() << " companies and " << analytics.document_count()
        << " bank analytical documents, " << news.size() << " news items\n";
    Metrics metrics((std::filesystem::path(work_dir) / "metrics.json").string());
    AppServices app{ data, analytics, agent, news, collector, metrics };
    const int result = run_http_server(server, app);
    curl_global_cleanup();
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}

