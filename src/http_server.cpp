#include "text_util.hpp"
#include "http_server.hpp"

#include "json.hpp"
#include "reader.hpp"
#include "llm_client.hpp"
#include "feed.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <climits>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t invalid_socket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t invalid_socket = -1;
static void close_socket(socket_t s) { close(s); }
#endif

namespace {
constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxBodyBytes = 2 * 1024 * 1024;
constexpr size_t kMaxActiveClients = 64;
std::atomic<size_t> active_clients{0};

struct Request {
    std::string method;
    std::string target;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct Response {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    std::string extra_headers;
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string percent_decode(const std::string& value, bool plus_as_space) {
    std::string out;
    out.reserve(value.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = hex(value[i + 1]);
            const int lo = hex(value[i + 2]);
            if (hi >= 0 && lo >= 0) { out.push_back(static_cast<char>((hi << 4) | lo)); i += 2; continue; }
        }
        out.push_back(plus_as_space && value[i] == '+' ? ' ' : value[i]);
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& target) {
    std::map<std::string, std::string> query;
    const auto q = target.find('?');
    if (q == std::string::npos) return query;
    size_t pos = q + 1;
    while (pos <= target.size()) {
        const size_t amp = target.find('&', pos);
        const std::string piece = target.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const size_t eq = piece.find('=');
        query[percent_decode(piece.substr(0, eq), true)] = eq == std::string::npos ? "" : percent_decode(piece.substr(eq + 1), true);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return query;
}

std::string path_only(const std::string& target) {
    const auto q = target.find('?');
    return target.substr(0, q);
}

bool safe_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

json::Value error_json(const std::string& code, const std::string& message) {
    json::Value out = json::Value::object();
    out["error"] = code;
    out["message"] = message;
    return out;
}

Response json_response(int status, const json::Value& value) {
    return Response{ status, "application/json; charset=utf-8", value.dump(), "" };
}

bool constant_time_equal(const std::string& a, const std::string& b) {
    size_t diff = a.size() ^ b.size();
    const size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= ca ^ cb;
    }
    return diff == 0;
}

bool receive_request(socket_t client, Request& request, int& error_status, std::string& error_message) {
    std::string wire;
    wire.reserve(8192);
    std::array<char, 8192> buffer{};
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const auto got = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (got <= 0) { error_status = 400; error_message = "Incomplete HTTP request"; return false; }
        wire.append(buffer.data(), static_cast<size_t>(got));
        if (wire.size() > kMaxHeaderBytes) { error_status = 431; error_message = "Request headers are too large"; return false; }
        header_end = wire.find("\r\n\r\n");
    }

    const std::string headers_text = wire.substr(0, header_end);
    std::istringstream lines(headers_text);
    std::string line;
    if (!std::getline(lines, line)) { error_status = 400; error_message = "Missing request line"; return false; }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream first(line);
    std::string version;
    if (!(first >> request.method >> request.target >> version) || version.rfind("HTTP/1.", 0) != 0) {
        error_status = 400; error_message = "Malformed request line"; return false;
    }
    if (request.target.empty() || request.target.front() != '/') { error_status = 400; error_message = "Only origin-form paths are accepted"; return false; }
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) { error_status = 400; error_message = "Malformed HTTP header"; return false; }
        request.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    if (request.headers.count("transfer-encoding")) { error_status = 400; error_message = "Transfer-Encoding is not supported"; return false; }
    size_t content_length = 0;
    if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
        try {
            size_t parsed = 0;
            const unsigned long long n = std::stoull(it->second, &parsed);
            if (parsed != it->second.size() || n > kMaxBodyBytes) { error_status = 413; error_message = "Request body is too large"; return false; }
            content_length = static_cast<size_t>(n);
        } catch (...) { error_status = 400; error_message = "Invalid Content-Length"; return false; }
    }
    request.body = wire.substr(header_end + 4);
    while (request.body.size() < content_length) {
        const size_t remaining = content_length - request.body.size();
        const int wanted = static_cast<int>(std::min(remaining, buffer.size()));
        const auto got = recv(client, buffer.data(), wanted, 0);
        if (got <= 0) { error_status = 400; error_message = "Incomplete request body"; return false; }
        request.body.append(buffer.data(), static_cast<size_t>(got));
    }
    if (request.body.size() > content_length) request.body.resize(content_length);
    return true;
}

std::string bearer(const Request& request) {
    const auto it = request.headers.find("authorization");
    if (it == request.headers.end()) {
        const auto key = request.headers.find("x-api-key");
        return key == request.headers.end() ? "" : key->second;
    }
    constexpr std::string_view prefix = "Bearer ";
    if (it->second.size() < prefix.size() || lower(it->second.substr(0, prefix.size())) != "bearer ") return "";
    return it->second.substr(prefix.size());
}

json::Value parse_body(const Request& request) {
    if (request.body.empty()) throw std::runtime_error("A JSON request body is required");
    const auto type = request.headers.find("content-type");
    if (type != request.headers.end() && lower(type->second).find("application/json") == std::string::npos)
        throw std::runtime_error("Content-Type must be application/json");
    return json::Value::parse(request.body);
}

const char* content_type_for(const std::string& ext) {
    static const std::pair<const char*, const char*> types[] = {
        { ".html", "text/html; charset=utf-8" }, { ".js", "text/javascript; charset=utf-8" }, { ".mjs", "text/javascript; charset=utf-8" },
        { ".css", "text/css; charset=utf-8" }, { ".json", "application/json; charset=utf-8" }, { ".svg", "image/svg+xml" },
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".webp", "image/webp" },
        { ".ico", "image/x-icon" }, { ".woff2", "font/woff2" }, { ".woff", "font/woff" }, { ".txt", "text/plain; charset=utf-8" },
        { ".map", "application/json; charset=utf-8" },
    };
    for (const auto& t : types) if (ext == t.first) return t.second;
    return "application/octet-stream";
}

// Serves a file from the frontend build directory. Rejects traversal, backslashes, NUL and drive letters;
// the resolved canonical path must stay inside the directory. Unknown routes without an extension
// fall back to index.html (single-page application).
bool serve_static(const std::string& root, const std::string& raw_path, Response& response) {
    namespace fs = std::filesystem;
    const std::string decoded = percent_decode(raw_path, false);
    if (decoded.empty() || decoded[0] != '/' || decoded.find('\\') != std::string::npos || decoded.find('\0') != std::string::npos ||
        decoded.find(':') != std::string::npos) {
        response = json_response(400, error_json("bad_path", "Path is not allowed"));
        return true;
    }
    for (size_t pos = 0; pos < decoded.size();) {
        const size_t next = decoded.find('/', pos + 1);
        const std::string segment = decoded.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
        if (segment == "..") { response = json_response(400, error_json("bad_path", "Path is not allowed")); return true; }
        if (next == std::string::npos) break;
        pos = next;
    }
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(fs::path(root), ec);
    if (ec) return false;
    fs::path target = fs::weakly_canonical(base / fs::path(decoded.substr(1)), ec);
    if (ec) return false;
    const std::string base_str = base.string(), target_str = target.string();
    if (target_str.compare(0, base_str.size(), base_str) != 0) { response = json_response(400, error_json("bad_path", "Path is not allowed")); return true; }
    if (decoded == "/" || fs::is_directory(target, ec)) target = target / "index.html";
    if (!fs::is_regular_file(target, ec)) {
        if (target.has_extension()) { response = json_response(404, error_json("not_found", "File was not found")); return true; }
        target = base / "index.html";
        if (!fs::is_regular_file(target, ec)) return false;
    }
    std::ifstream in(target, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    response.status = 200;
    response.content_type = content_type_for(target.extension().string());
    response.body = buffer.str();
    return true;
}

Response route(const Request& request, const ServerConfig& config, AppServices& app) {
    const DataStore& data = app.data;
    const AnalyticsIndex& analytics = app.analytics;
    InsightAgent& agent = app.agent;
    const std::string path = path_only(request.target);
    const auto query = parse_query(request.target);
    if (request.method == "GET" && path == "/health") {
        json::Value value = json::Value::object();
        value["status"] = "ok";
        value["service"] = "alpha-news-backend";
        return json_response(200, value);
    }
    if (request.method == "OPTIONS") return Response{ 204, "text/plain; charset=utf-8", "", "" };
    if (request.method == "GET" && !config.static_dir.empty() && path.rfind("/api/", 0) != 0) {
        Response file;
        if (serve_static(config.static_dir, path, file)) return file;
    }
    if (!config.api_token.empty() && !constant_time_equal(bearer(request), config.api_token))
        return json_response(401, error_json("unauthorized", "Provide a valid bearer token or x-api-key"));

    if (request.method == "GET" && path == "/api/v1/models") {
        json::Value value = json::Value::object();
        value["configured"] = LlmConfig::from_environment().configured();
        value["models"] = json::Value::array();
        for (const auto& model : allowed_llm_models()) value["models"].push_back(model);
        return json_response(200, value);
    }
    if (request.method == "GET" && path == "/api/v1/clients") {
        ClientFilter filter;
        const auto text = [&](const char* key) { const auto it = query.find(key); return it == query.end() ? std::string() : it->second; };
        filter.query = text("q").substr(0, 200);
        filter.industry = text("industry").substr(0, 400);
        filter.region = text("region").substr(0, 200);
        try { if (!text("min_revenue").empty()) filter.min_revenue = std::max(0.0, std::stod(text("min_revenue"))); } catch (...) {}
        try { if (!text("limit").empty()) filter.limit = std::clamp<size_t>(std::stoul(text("limit")), 1, 200); } catch (...) {}
        try { if (!text("offset").empty()) filter.offset = std::min<size_t>(std::stoul(text("offset")), 1000000); } catch (...) {}
        return json_response(200, data.list_clients(filter));
    }
    if (request.method == "GET" && path == "/api/v1/clients/facets") {
        return json_response(200, data.client_facets(15000000000.0));
    }
    if (request.method == "GET" && path == "/api/v1/companies") {
        const auto it = query.find("q");
        if (it == query.end() || it->second.empty()) return json_response(400, error_json("query_required", "Provide q with a company name or identifier"));
        json::Value result = json::Value::object();
        result["items"] = json::Value::array();
        size_t limit = 20;
        if (const auto lim = query.find("limit"); lim != query.end()) {
            try { limit = std::clamp<size_t>(std::stoul(lim->second), 1, 50); } catch (...) {}
        }
        for (const auto* company : data.search(it->second, limit)) result["items"].push_back(data.profile_json(*company));
        return json_response(200, result);
    }
    if (request.method == "GET" && path == "/api/v1/roles") {
        json::Value value = json::Value::object();
        value["items"] = roles_json();
        return json_response(200, value);
    }
    if (request.method == "GET" && path == "/api/v1/news") {
        size_t limit = 50;
        if (const auto lim = query.find("limit"); lim != query.end()) {
            try { limit = std::clamp<size_t>(std::stoul(lim->second), 1, 500); } catch (...) {}
        }
        json::Value value = json::Value::object();
        value["total"] = static_cast<long long>(app.news.size());
        value["items"] = json::Value::array();
        for (const auto& item : app.news.recent(limit)) value["items"].push_back(news_to_json(item));
        return json_response(200, value);
    }
    if (request.method == "POST" && path == "/api/v1/news/refresh") {
        return json_response(200, app.collector.refresh_now(true));
    }
    if (request.method == "GET" && path.rfind("/api/v1/companies/", 0) == 0 && path.size() > 5 &&
        path.compare(path.size() - 5, 5, "/feed") == 0) {
        const std::string id = percent_decode(path.substr(18, path.size() - 18 - 5), false);
        if (!safe_id(id)) return json_response(400, error_json("invalid_company_id", "Company id contains unsupported characters"));
        const auto* company = data.find(id);
        if (!company) return json_response(404, error_json("company_not_found", "Company id was not found in the dataset"));
        const auto role_it = query.find("role");
        const std::string role = role_it == query.end() ? "ceo" : role_it->second;
        if (!role.empty() && !find_role(role)) return json_response(400, error_json("unknown_role", "Use one of /api/v1/roles"));
        size_t limit = 20;
        if (const auto lim = query.find("limit"); lim != query.end()) {
            try { limit = std::clamp<size_t>(std::stoul(lim->second), 1, 100); } catch (...) {}
        }
        app.metrics.record_feed(role);
        return json_response(200, build_feed(data, analytics, app.news, *company, role, limit));
    }
    if (request.method == "GET" && path.rfind("/api/v1/companies/", 0) == 0) {
        std::string tail = path.substr(std::string("/api/v1/companies/").size());
        bool network = false;
        constexpr std::string_view suffix = "/network";
        if (tail.size() > suffix.size() && tail.compare(tail.size() - suffix.size(), suffix.size(), suffix) == 0) {
            tail.resize(tail.size() - suffix.size()); network = true;
        }
        const std::string id = percent_decode(tail, false);
        if (!safe_id(id)) return json_response(400, error_json("invalid_company_id", "Company id contains unsupported characters"));
        const auto* company = data.find(id);
        if (!company) return json_response(404, error_json("company_not_found", "Company id was not found in the dataset"));
        return json_response(200, network ? data.network_json(*company) : data.profile_json(*company));
    }
    if (request.method == "GET" && path == "/api/v1/analytics/search") {
        const auto it = query.find("q");
        if (it == query.end() || it->second.empty()) return json_response(400, error_json("query_required", "Provide q to search bank analytics"));
        size_t limit = 5;
        if (const auto lim = query.find("limit"); lim != query.end()) {
            try { limit = std::clamp<size_t>(std::stoul(lim->second), 1, 20); } catch (...) {}
        }
        json::Value result = json::Value::object();
        result["items"] = json::Value::array();
        for (const auto& hit : analytics.search(it->second, limit)) {
            json::Value item = json::Value::object();
            item["id"] = hit.id; item["document"] = hit.document; item["page"] = hit.page;
            item["score"] = hit.score; item["text"] = hit.text;
            item["text_extraction"] = hit.extraction_method;
            result["items"].push_back(std::move(item));
        }
        return json_response(200, result);
    }
    if (request.method == "POST" && path == "/api/v1/parse") {
        json::Value body;
        try { body = parse_body(request); } catch (const std::exception& e) { return json_response(400, error_json("invalid_json", e.what())); }
        const std::string url = body.get("url").as_string();
        if (url.empty()) return json_response(400, error_json("url_required", "Provide a public http/https URL"));
        const std::string mode = body.get("mode").as_string();
        const ParsedPage page = read_url(url, 25, mode != "raw");
        if (!page.ok()) return json_response(422, error_json("parse_failed", page.error));
        json::Value result = json::Value::object();
        result["url"] = page.url; result["title"] = page.title; result["published_time"] = page.published_time;
        result["http_status"] = static_cast<long long>(page.http_status); result["markdown"] = page.jina_markdown;
        result["content_markdown"] = page.content_markdown;
        return json_response(200, result);
    }
    if (request.method == "POST" && path == "/api/v1/insights") {
        json::Value body;
        try { body = parse_body(request); } catch (const std::exception& e) { return json_response(400, error_json("invalid_json", e.what())); }
        const std::string company_id = body.get("company_id").as_string();
        if (!safe_id(company_id)) return json_response(400, error_json("invalid_company_id", "Provide a valid company_id"));
        if (!data.find(company_id)) return json_response(404, error_json("company_not_found", "Company id was not found in the dataset"));
        ArticleInput article;
        article.url = body.get("url").as_string();
        article.title = body.get("title").as_string();
        article.published_time = body.get("published_time").as_string();
        article.source_name = body.get("source_name").as_string();
        article.text = body.get("text").as_string();
        const std::string news_id = body.get("news_id").as_string();
        if (!news_id.empty()) {
            if (!safe_id(news_id)) return json_response(400, error_json("invalid_news_id", "news_id is invalid"));
            NewsItem item;
            if (!app.news.find(news_id, item)) return json_response(404, error_json("news_not_found", "news_id was not found in the news store"));
            article.news_id = item.id;
            article.title = item.title;
            article.text = item.summary.empty() ? item.title : item.summary;
            article.url = item.url;
            article.published_time = item.published;
            article.source_name = item.source;
            article.categories = item.categories;
        }
        const std::string role = body.get("role").as_string();
        if (!role.empty() && !find_role(role)) return json_response(400, error_json("unknown_role", "Use one of /api/v1/roles"));
        if (news_id.empty() && !article.url.empty()) {
            const ParsedPage page = read_url(article.url, 25, true);
            if (!page.ok()) return json_response(422, error_json("parse_failed", page.error));
            if (article.title.empty()) article.title = page.title;
            if (article.published_time.empty()) article.published_time = page.published_time;
            if (article.text.empty()) article.text = page.content_markdown;
            if (article.source_name.empty()) article.source_name = "Внешняя новость";
        }
        if (article.text.empty()) return json_response(400, error_json("article_required", "Provide news_id, url or text"));
        if (article.text.size() > 30000 || article.title.size() > 1000 || article.url.size() > 4096)
            return json_response(413, error_json("article_too_large", "Article text or metadata exceeds the limit"));
        const auto insight = agent.analyze(company_id, article, role.empty() ? "ceo" : role);
        const auto insight_error = insight.get("error").as_string();
        if (insight_error.empty()) app.metrics.record_insight(insight);
        return json_response(insight_error.empty() ? 200 : insight_error == "company_not_found" ? 404 : 400, insight);
    }
    if (request.method == "POST" && path == "/api/v1/feedback") {
        json::Value body;
        try { body = parse_body(request); } catch (const std::exception& e) { return json_response(400, error_json("invalid_json", e.what())); }
        const std::string id = body.get("insight_id").as_string();
        if (!safe_id(id)) return json_response(400, error_json("invalid_insight_id", "Provide insight_id"));
        if (!body.get("useful").is_bool()) return json_response(400, error_json("useful_required", "useful must be true or false"));
        json::Value insight;
        if (!agent.find_insight(id, insight)) return json_response(404, error_json("insight_not_found", "Insight was not found in this process"));
        const std::string reason = textutil::truncate_utf8(body.get("reason").as_string(), 500);
        app.metrics.record_feedback(id, body.get("useful").as_bool(), reason);
        json::Value value = json::Value::object();
        value["status"] = "recorded";
        return json_response(200, value);
    }
    if (request.method == "GET" && path == "/api/v1/metrics") {
        return json_response(200, app.metrics.snapshot(agent.budget()));
    }
    const std::string follow_prefix = "/api/v1/insights/";
    constexpr std::string_view questions_suffix = "/questions";
    if (request.method == "POST" && path.rfind(follow_prefix, 0) == 0 && path.size() > follow_prefix.size() + questions_suffix.size() &&
        path.compare(path.size() - questions_suffix.size(), questions_suffix.size(), questions_suffix) == 0) {
        const std::string id = percent_decode(path.substr(follow_prefix.size(), path.size() - follow_prefix.size() - questions_suffix.size()), false);
        if (!safe_id(id)) return json_response(400, error_json("invalid_insight_id", "Insight id is invalid"));
        json::Value body;
        try { body = parse_body(request); } catch (const std::exception& e) { return json_response(400, error_json("invalid_json", e.what())); }
        const std::string question = body.get("question").as_string();
        if (question.size() > 2000) return json_response(413, error_json("question_too_large", "Question exceeds 2000 characters"));
        const std::string lens = body.get("lens").as_string();
        if (!lens.empty() && lens != "risks" && lens != "counterparties" && lens != "treasury" && lens != "scenarios" && lens != "sources")
            return json_response(400, error_json("unknown_lens", "lens must be one of risks, counterparties, treasury, scenarios, sources"));
        const json::Value answer = agent.follow_up(id, question, lens);
        const auto error = answer.get("error").as_string();
        return json_response(error.empty() ? 200 : error == "question_empty" ? 400 : 404, answer);
    }
    return json_response(404, error_json("not_found", "Route was not found"));
}

const char* reason_phrase(int status) {
    switch (status) {
    case 200: return "OK"; case 204: return "No Content"; case 400: return "Bad Request"; case 401: return "Unauthorized";
    case 404: return "Not Found"; case 413: return "Payload Too Large"; case 422: return "Unprocessable Entity";
    case 431: return "Request Header Fields Too Large"; case 500: return "Internal Server Error";
    case 503: return "Service Unavailable"; default: return "Error";
    }
}

bool send_all(socket_t client, const std::string& value) {
    size_t sent = 0;
    while (sent < value.size()) {
#ifdef _WIN32
        const int count = send(client, value.data() + sent, static_cast<int>(std::min<size_t>(value.size() - sent, INT_MAX)), 0);
#else
        const auto count = send(client, value.data() + sent, value.size() - sent, MSG_NOSIGNAL);
#endif
        if (count <= 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

void write_response(socket_t client, const Response& response) {
    std::ostringstream head;
    head << "HTTP/1.1 " << response.status << ' ' << reason_phrase(response.status) << "\r\n"
        << "Content-Type: " << response.content_type << "\r\n"
        << "Content-Length: " << response.body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "X-Content-Type-Options: nosniff\r\n"
        << "Referrer-Policy: no-referrer\r\n"
        << response.extra_headers
        << "Connection: close\r\n\r\n";
    send_all(client, head.str() + response.body);
}

void serve_one(socket_t client, const ServerConfig& config, AppServices& app) {
    const auto started = std::chrono::steady_clock::now();
    Request request;
    int status = 500;
    Response response;
    int parse_status = 400;
    std::string parse_error;
    if (!receive_request(client, request, parse_status, parse_error)) {
        response = json_response(parse_status, error_json("bad_request", parse_error));
    } else {
        try {
            response = route(request, config, app);
        } catch (const std::exception&) {
            response = json_response(500, error_json("internal_error", "Request could not be processed"));
        } catch (...) {
            response = json_response(500, error_json("internal_error", "Request could not be processed"));
        }
    }
    if (!config.cors_origin.empty()) {
        response.extra_headers += "Access-Control-Allow-Origin: " + config.cors_origin + "\r\n"
            "Vary: Origin\r\nAccess-Control-Allow-Headers: Content-Type, Authorization, x-api-key\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Max-Age: 600\r\n";
    }
    status = response.status;
    write_response(client, response);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cerr << "http method=" << (request.method.empty() ? "?" : request.method)
        << " status=" << status << " duration_ms=" << elapsed << '\n';
    close_socket(client);
    --active_clients;
}
} // namespace

int run_http_server(const ServerConfig& config, AppServices& app) {
    socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == invalid_socket) { std::cerr << "Could not create HTTP socket\n"; return 1; }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.host.c_str(), &address.sin_addr) != 1) {
        std::cerr << "ALFA_HOST must be an IPv4 address\n"; close_socket(listener); return 1;
    }
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 64) != 0) {
        std::cerr << "Could not bind HTTP server to " << config.host << ':' << config.port << "\n";
        close_socket(listener); return 1;
    }
    std::cout << "Alpha News Backend listening on " << config.host << ':' << config.port << '\n';
    for (;;) {
        sockaddr_in peer{};
#ifdef _WIN32
        int peer_size = sizeof(peer);
#else
        socklen_t peer_size = sizeof(peer);
#endif
        socket_t client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (client == invalid_socket) continue;
#ifdef _WIN32
        DWORD timeout = 15000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{15, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
        if (++active_clients > kMaxActiveClients) {
            --active_clients;
            write_response(client, json_response(503, error_json("busy", "Server concurrency limit reached")));
            close_socket(client);
            continue;
        }
        std::thread(serve_one, client, std::cref(config), std::ref(app)).detach();
    }
}

