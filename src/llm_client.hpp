#pragma once

#include <string>
#include <vector>

struct LlmConfig {
    std::string base_url;
    std::string api_key;
    std::string model;          // main analysis model
    std::string fast_model;     // planning and follow-up questions
    std::string review_model;   // self-check
    int request_timeout_seconds = 35;

    static LlmConfig from_environment();
    bool configured() const { return !api_key.empty(); }
};

struct LlmReply {
    bool ok = false;
    std::string content;
    std::string error;
    std::string model;
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
};

const std::vector<std::string>& allowed_llm_models();
bool allowed_llm_base_url(const std::string& url);
LlmReply llm_chat(const LlmConfig& config, const std::string& system_prompt,
    const std::string& user_prompt, int max_tokens = 1600);
// Same as llm_chat but with an explicit model from the allowlist.
LlmReply llm_chat_model(const LlmConfig& config, const std::string& model, const std::string& system_prompt,
    const std::string& user_prompt, int max_tokens = 1600);
// True only when ALFA_LLM_TEST_ENDPOINT=1: allows http://127.0.0.1:<port>/v1 for automated tests.
bool llm_test_endpoint_enabled();
