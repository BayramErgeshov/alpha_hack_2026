#include "../src/json.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace {
struct Options {
    std::string base = "http://127.0.0.1:18080";
    fs::path output = "output";
    fs::path requests = "requests";
    bool web = false;
};
std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read request file: " + path.filename().string());
    std::string text((std::istreambuf_iterator<char>(stream)), {});
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    if (text.size() > 65536) throw std::runtime_error("Example request file is too large");
    return text;
}
void write_json(const fs::path& path, const json::Value& value) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || !(stream << value.dump() << '\n')) throw std::runtime_error("Cannot save example response");
}
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
std::string url_part(CURLU* url, CURLUPart part) {
    char* raw = nullptr;
    if (curl_url_get(url, part, &raw, 0) != CURLUE_OK) return {};
    std::string result(raw); curl_free(raw); return result;
}
std::string local_base(const std::string& base) {
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, base.c_str(), 0) != CURLUE_OK)
        throw std::runtime_error("Invalid --base-url");
    const auto scheme = lower(url_part(url.get(), CURLUPART_SCHEME));
    const auto host = lower(url_part(url.get(), CURLUPART_HOST));
    const auto path = url_part(url.get(), CURLUPART_PATH);
    if ((scheme != "http" && scheme != "https") || (host != "localhost" && host != "127.0.0.1") ||
        !url_part(url.get(), CURLUPART_USER).empty() || !url_part(url.get(), CURLUPART_PASSWORD).empty() ||
        !url_part(url.get(), CURLUPART_QUERY).empty() || !url_part(url.get(), CURLUPART_FRAGMENT).empty() ||
        (path != "/" && !path.empty()))
        throw std::runtime_error("--base-url must be an http(s) loopback origin without credentials, path, query or fragment");
    // Pin localhost to the loopback address regardless of machine DNS/proxy settings.
    if (host == "localhost") curl_url_set(url.get(), CURLUPART_HOST, "127.0.0.1", 0);
    std::string result = url_part(url.get(), CURLUPART_URL);
    while (!result.empty() && result.back() == '/') result.pop_back();
    return result;
}
size_t receive(char* bytes, size_t size, size_t count, void* destination) {
    auto& text = *static_cast<std::string*>(destination);
    const size_t length = size * count;
    if (length > 4 * 1024 * 1024 || text.size() > 4 * 1024 * 1024 - length) return 0;
    text.append(bytes, length); return length;
}
struct Reply { long status = 0; json::Value body; };
class Client {
    std::string base_, token_;
public:
    explicit Client(std::string base) : base_(std::move(base)) {
#ifdef _WIN32
        char* token = nullptr;
        size_t length = 0;
        if (_dupenv_s(&token, &length, "ALFA_EXAMPLE_API_TOKEN") == 0 && token) token_ = token;
        if (token) { std::fill_n(token, length, '\0'); std::free(token); }
#else
        if (const char* token = std::getenv("ALFA_EXAMPLE_API_TOKEN")) token_ = token;
#endif
        if (token_.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Invalid example API token format");
    }
    Reply request(const std::string& method, const std::string& route, const std::string& body) const {
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) throw std::runtime_error("Cannot initialize HTTP client");
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/json");
        if (method == "POST") headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
        if (!token_.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + token_).c_str());
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> owned_headers(headers, curl_slist_free_all);
        const std::string url = base_ + route;
        std::string response;
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 40L);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
        if (method == "POST") {
            curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        }
        if (curl_easy_perform(curl.get()) != CURLE_OK) throw std::runtime_error("Local HTTP request failed; check the example server and its logs");
        Reply reply;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &reply.status);
        try { reply.body = json::Value::parse(response); }
        catch (...) { throw std::runtime_error("Server returned invalid JSON"); }
        return reply;
    }
};
std::string percent_encode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') encoded += static_cast<char>(c);
        else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
    }
    return encoded;
}
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool has_source(const json::Value& body, const std::string& id) {
    for (const auto& source : body.get("sources").as_array()) if (source.get("id").as_string() == id) return true;
    return false;
}
void validate_insight(const json::Value& body, const std::string& company) {
    require(!body.get("id").as_string().empty(), "Insight id is missing");
    require(body.get("company_id").as_string() == company, "Insight company does not match the request");
    require(body.get("generation").get("method").as_string() == "deterministic_fallback", "Expected deterministic analysis without model calls");
    require(body.get("quality_check").get("llm_used").is_bool() && !body.get("quality_check").get("llm_used").as_bool(), "Unexpected LLM usage");
    require(has_source(body, "C1") && has_source(body, "N1"), "Insight lacks company/news source references");
}
json::Value load_request(const fs::path& dir, const char* name, bool permit_url = false) {
    auto body = json::Value::parse(read_file(dir / name));
    require(body.is_object(), "Example request must be a JSON object");
    if (!permit_url) require(body.get("url").as_string().empty(), "Default examples must use embedded news text without external requests");
    return body;
}
Options options(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--include-web") opt.web = true;
        else if (argument == "--help") {
            std::cout << "AlphaExampleClient.exe --base-url http://127.0.0.1:18080 --output-dir PATH --requests-dir PATH [--include-web]\n"
                         "Uses ALFA_EXAMPLE_API_TOKEN only. Requires a server with models.configured=false.\n";
            std::exit(0);
        } else {
            require(i + 1 < argc, "Option requires a value");
            const std::string value = argv[++i];
            if (argument == "--base-url") opt.base = value;
            else if (argument == "--output-dir") opt.output = fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
            else if (argument == "--requests-dir") opt.requests = fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
            else throw std::runtime_error("Unknown command line option");
        }
    }
    return opt;
}
} // namespace

int example_main(int argc, char** argv) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { std::cerr << "Cannot initialize libcurl\n"; return 1; }
    struct Cleanup { ~Cleanup() { curl_global_cleanup(); } } cleanup;
    json::Value summary = json::Value::object();
    summary["scenarios"] = json::Value::array();
    summary["model_call_count"] = 0;
    summary["passed"] = false;
    fs::path output;
    int number = 0;
    try {
        const auto opt = options(argc, argv);
        output = opt.output;
        fs::create_directories(output);
        Client client(local_base(opt.base));
        auto run = [&](const std::string& name, const std::string& method, const std::string& route, const json::Value& body,
                       long expected, auto validate) -> json::Value {
            json::Value item = json::Value::object();
            item["name"] = name; item["method"] = method; item["route"] = route;
            item["expected_status"] = static_cast<long long>(expected); item["status"] = nullptr;
            item["output_file"] = nullptr; item["passed"] = false;
            try {
                const Reply response = client.request(method, route, method == "POST" ? body.dump() : "");
                item["status"] = static_cast<long long>(response.status);
                const std::string filename = (++number < 10 ? "0" : "") + std::to_string(number) + "-" + name + ".json";
                write_json(output / filename, response.body); item["output_file"] = filename;
                require(response.status == expected, "Unexpected HTTP status");
                validate(response.body);
                item["passed"] = true;
                summary["scenarios"].push_back(item);
                std::cout << "[OK] " << name << " HTTP " << response.status << '\n';
                return response.body;
            } catch (const std::exception& error) {
                item["validation_error"] = error.what();
                summary["scenarios"].push_back(item);
                throw std::runtime_error("Scenario failed: " + name + ". " + error.what());
            }
        };
        const auto nothing = [](const json::Value&) {};
        run("health", "GET", "/health", nullptr, 200, [](const auto& v) { require(v.get("status").as_string() == "ok", "Server is not healthy"); });
        run("models", "GET", "/api/v1/models", nullptr, 200, [](const auto& v) {
            require(v.get("configured").is_bool() && !v.get("configured").as_bool(), "Stop: model access must be disabled for examples");
            require(!v.get("models").as_array().empty(), "Model allowlist is missing");
        });
        const auto credit = load_request(opt.requests, "insight-credit.json");
        const auto trade = load_request(opt.requests, "insight-trade.json");
        const auto question = load_request(opt.requests, "question.json");
        run("company-search", "GET", "/api/v1/companies?q=U_9250&limit=5", nullptr, 200, [](const auto& v) {
            bool found = false; for (const auto& item : v.get("items").as_array()) if (item.get("id").as_string() == "U_9250") found = true;
            require(found, "Example company was not found");
        });
        run("company-profile", "GET", "/api/v1/companies/U_9250", nullptr, 200, [](const auto& v) { require(v.get("id").as_string() == "U_9250", "Wrong company profile"); });
        run("company-network", "GET", "/api/v1/companies/U_9250/network", nullptr, 200, [](const auto& v) { require(v.get("company_id").as_string() == "U_9250", "Wrong company network"); });
        run("analytics-search", "GET", "/api/v1/analytics/search?q=" + percent_encode("инфляция ставка") + "&limit=3", nullptr, 200, [](const auto& v) { require(!v.get("items").as_array().empty(), "No analytics fragments found; check the data folder"); });
        const auto first = run("insight-credit", "POST", "/api/v1/insights", credit, 200, [&](const auto& v) { validate_insight(v, credit.get("company_id").as_string()); });
        const auto second = run("insight-trade", "POST", "/api/v1/insights", trade, 200, [&](const auto& v) { validate_insight(v, trade.get("company_id").as_string()); });
        require(first.get("id").as_string() != second.get("id").as_string(), "Insight ids must be unique in the current run");
        const std::string question_route = "/api/v1/insights/" + percent_encode(first.get("id").as_string()) + "/questions";
        run("follow-up", "POST", question_route, question, 200, [&](const auto& v) {
            require(v.get("insight_id").as_string() == first.get("id").as_string(), "Follow-up id does not match the current insight");
            require(v.get("method").as_string() == "deterministic_fallback", "Unexpected model follow-up");
            require(!v.get("answer").as_string().empty(), "Follow-up answer is empty");
        });
        run("missing-company", "GET", "/api/v1/companies/U_999999999", nullptr, 404, [](const auto& v) { require(v.get("error").as_string() == "company_not_found", "Wrong missing-company error"); });
        const json::Value empty = load_request(opt.requests, "empty-question.json");
        require(empty.get("question").is_string() && empty.get("question").as_string().find_first_not_of(" \t\r\n") == std::string::npos,
            "empty-question.json must contain an empty or whitespace-only question");
        run("empty-question", "POST", question_route, empty, 400, [](const auto& v) { require(v.get("error").as_string() == "question_empty", "Wrong empty-question error"); });
        const json::Value local = load_request(opt.requests, "blocked-url.json", true);
        // The fixture must remain a loopback origin, which read_url rejects before fetching.
        (void)local_base(local.get("url").as_string());
        run("blocked-local-url", "POST", "/api/v1/parse", local, 422, [](const auto& v) { require(v.get("error").as_string() == "parse_failed", "Expected local URL rejection"); });
        if (opt.web) {
            const auto request = load_request(opt.requests, "parse-url.json", true);
            require(request.get("url").as_string() == "https://example.com", "Web example is restricted to https://example.com");
            run("parse-web", "POST", "/api/v1/parse", request, 200, [](const auto& v) {
                require(v.get("markdown").as_string().find("Markdown Content:") != std::string::npos, "Jina-style markdown is missing");
            });
        }
        summary["passed"] = true;
        summary["web_example_included"] = opt.web;
        write_json(output / "summary.json", summary);
        std::cout << "Completed: " << summary.get("scenarios").as_array().size() << " scenarios passed. Responses and summary.json saved to the output directory. Model calls: 0.\n";
        return 0;
    } catch (const std::exception& e) {
        if (!output.empty()) { try { write_json(output / "summary.json", summary); } catch (...) {} }
        std::cerr << e.what() << '\n';
        std::cerr << "Examples failed. Completed scenario records: " << summary.get("scenarios").as_array().size() << ". Model calls: 0.\n";
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wide_argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0) { std::cerr << "Cannot read command line as Unicode\n"; return 1; }
        std::string encoded(static_cast<size_t>(length), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, encoded.data(), length, nullptr, nullptr)) return 1;
        encoded.pop_back();
        arguments.push_back(std::move(encoded));
    }
    std::vector<char*> argv;
    argv.reserve(static_cast<size_t>(argc));
    for (auto& argument : arguments) argv.push_back(argument.data());
    return example_main(argc, argv.data());
}
#else
int main(int argc, char** argv) { return example_main(argc, argv); }
#endif
