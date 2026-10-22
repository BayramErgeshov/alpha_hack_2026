#pragma once

#include <string>
#include <vector>

struct LlmConfig {
    std::string base_url;
    std::string api_key;
    std::string model;
    int request_timeout_seconds = 35;

    static LlmConfig from_environment();
    bool configured() const { return !api_key.empty(); }
};

struct LlmReply {
    bool ok = false;
    std::string content;
    std::string error;
    std::string model;
};

const std::vector<std::string>& allowed_llm_models();
bool allowed_llm_base_url(const std::string& url);
LlmReply llm_chat(const LlmConfig& config, const std::string& system_prompt,
    const std::string& user_prompt, int max_tokens = 1600);
