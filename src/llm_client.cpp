#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#include "llm_client.hpp"
#include "json.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstdlib>
#include <string_view>

namespace {

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

size_t append_response(char* data, size_t size, size_t count, void* userdata) {
    auto* response = static_cast<std::string*>(userdata);
    const size_t bytes = size * count;
    constexpr size_t limit = 2 * 1024 * 1024;
    if (bytes > limit - std::min(response->size(), limit)) return 0;
    response->append(data, bytes);
    return bytes;
}

std::string trim_slash(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

std::string api_url(std::string base) {
    base = trim_slash(std::move(base));
    if (base.size() >= 3 && base.substr(base.size() - 3) == "/v1")
        return base + "/chat/completions";
    return base + "/v1/chat/completions";
}

} // namespace

bool llm_test_endpoint_enabled() {
    return env("ALFA_LLM_TEST_ENDPOINT") == "1";
}

const std::vector<std::string>& allowed_llm_models() {
    static const std::vector<std::string> models = {
        "google/gemma-4-26b-a4b-it",
        "z-ai/glm-5.3-flash",
        "qwen/qwen3.6-35b-a3b",
        "deepseek/deepseek-v4.1-flash",
        "qwen/qwen3-vl-30b-a3b-instruct",
        "moonshotai/kimi-k2.6"
    };
    return models;
}

bool allowed_llm_base_url(const std::string& url) {
    CURLU* parsed = curl_url();
    if (!parsed) return false;
    char* scheme = nullptr;
    char* host = nullptr;
    char* path = nullptr;
    char* port = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    char* query = nullptr;
    char* fragment = nullptr;
    const bool valid = curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_PATH, &path, 0) == CURLUE_OK;
    const bool has_port = curl_url_get(parsed, CURLUPART_PORT, &port, 0) == CURLUE_OK;
    const bool has_user = curl_url_get(parsed, CURLUPART_USER, &user, 0) == CURLUE_OK;
    const bool has_password = curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0) == CURLUE_OK;
    const bool has_query = curl_url_get(parsed, CURLUPART_QUERY, &query, 0) == CURLUE_OK;
    const bool has_fragment = curl_url_get(parsed, CURLUPART_FRAGMENT, &fragment, 0) == CURLUE_OK;
    const std::string scheme_value = scheme ? scheme : "";
    const std::string host_value = host ? host : "";
    const std::string path_value = path ? path : "";
    const std::string port_value = port ? port : "";
    if (scheme) curl_free(scheme);
    if (host) curl_free(host);
    if (path) curl_free(path);
    if (port) curl_free(port);
    if (user) curl_free(user);
    if (password) curl_free(password);
    if (query) curl_free(query);
    if (fragment) curl_free(fragment);
    curl_url_cleanup(parsed);
    const bool allowed_host = host_value == "changellenge.up.railway.app" || host_value == "api.alfahack-gateway26.ru";
    const bool allowed_path = path_value.empty() || path_value == "/" || path_value == "/v1" || path_value == "/v1/";
    const bool allowed_port = !has_port || port_value == "443";
    const bool clean = valid && allowed_path && !has_user && !has_password && !has_query && !has_fragment;
    if (clean && llm_test_endpoint_enabled() && scheme_value == "http" && host_value == "127.0.0.1") return true;
    return clean && scheme_value == "https" && allowed_host && allowed_port;
}

LlmConfig LlmConfig::from_environment() {
    LlmConfig config;
    config.base_url = env("ALFA_LLM_BASE_URL");
    if (config.base_url.empty()) config.base_url = "https://changellenge.up.railway.app/v1";
    config.api_key = env("ALFA_LLM_API_KEY");
    if (config.api_key.empty()) config.api_key = env("OPENAI_API_KEY");
    config.model = env("ALFA_LLM_MODEL");
    if (config.model.empty()) config.model = "deepseek/deepseek-v4.1-flash";
    config.fast_model = env("ALFA_LLM_MODEL_FAST");
    if (config.fast_model.empty()) config.fast_model = "google/gemma-4-26b-a4b-it";
    config.review_model = env("ALFA_LLM_MODEL_REVIEW");
    if (config.review_model.empty()) config.review_model = "z-ai/glm-5.3-flash";
    const std::string timeout = env("ALFA_LLM_TIMEOUT_SECONDS");
    if (!timeout.empty()) {
        try { config.request_timeout_seconds = std::clamp(std::stoi(timeout), 5, 90); }
        catch (...) {}
    }
    return config;
}

LlmReply llm_chat(const LlmConfig& config, const std::string& system_prompt,
    const std::string& user_prompt, int max_tokens) {
    return llm_chat_model(config, config.model, system_prompt, user_prompt, max_tokens);
}

LlmReply llm_chat_model(const LlmConfig& base_config, const std::string& model, const std::string& system_prompt,
    const std::string& user_prompt, int max_tokens) {
    LlmConfig config = base_config;
    config.model = model;
    LlmReply reply;
    reply.model = config.model;
    if (!config.configured()) { reply.error = "LLM API key is not configured"; return reply; }
    if (!allowed_llm_base_url(config.base_url)) {
        reply.error = "LLM base URL must use one of the two endpoints supplied for this case";
        return reply;
    }
    const auto& models = allowed_llm_models();
    if (std::find(models.begin(), models.end(), config.model) == models.end()) {
        reply.error = "The configured model is not on the case allowlist";
        return reply;
    }

    json::Value payload = json::Value::object();
    payload["model"] = config.model;
    payload["temperature"] = 0.15;
    payload["max_tokens"] = std::clamp(max_tokens, 256, 3000);
    json::Value messages = json::Value::array();
    json::Value system = json::Value::object();
    system["role"] = "system";
    system["content"] = system_prompt;
    messages.push_back(std::move(system));
    json::Value user = json::Value::object();
    user["role"] = "user";
    user["content"] = user_prompt;
    messages.push_back(std::move(user));
    payload["messages"] = std::move(messages);
    const std::string body = payload.dump();

    CURL* curl = curl_easy_init();
    if (!curl) { reply.error = "Could not initialize the HTTP client"; return reply; }
    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    const std::string authorization = "Authorization: Bearer " + config.api_key;
    headers = curl_slist_append(headers, authorization.c_str());
    const std::string endpoint = api_url(config.base_url);
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, std::clamp(config.request_timeout_seconds, 5, 90));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    const bool plain_http_test = llm_test_endpoint_enabled() && config.base_url.rfind("http://127.0.0.1", 0) == 0;
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, plain_http_test ? "http" : "https");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AlfaNewsBackend/0.1");

    const CURLcode request_status = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (request_status != CURLE_OK) {
        reply.error = curl_easy_strerror(request_status);
        return reply;
    }
    if (http_status < 200 || http_status >= 300) {
        reply.error = "LLM gateway returned HTTP " + std::to_string(http_status);
        return reply;
    }

    try {
        const json::Value root = json::Value::parse(response);
        const auto& choices = root.get("choices").as_array();
        if (choices.empty()) { reply.error = "LLM gateway response has no choices"; return reply; }
        const json::Value& content = choices.front().get("message").get("content");
        if (content.is_string()) reply.content = content.as_string();
        else if (content.is_array()) {
            for (const auto& part : content.as_array()) {
                const auto& text = part.get("text");
                if (text.is_string()) reply.content += text.as_string();
            }
        }
        if (reply.content.empty()) { reply.error = "LLM gateway returned an empty message"; return reply; }
        const json::Value& usage = root.get("usage");
        reply.prompt_tokens = static_cast<long long>(usage.get("prompt_tokens").as_number());
        reply.completion_tokens = static_cast<long long>(usage.get("completion_tokens").as_number());
        if (reply.prompt_tokens <= 0 && reply.completion_tokens <= 0) {
            reply.prompt_tokens = static_cast<long long>((system_prompt.size() + user_prompt.size()) / 3);
            reply.completion_tokens = static_cast<long long>(reply.content.size() / 3);
        }
        reply.ok = true;
        return reply;
    } catch (const std::exception& e) {
        reply.error = std::string("Could not parse LLM gateway response: ") + e.what();
        return reply;
    }
}
