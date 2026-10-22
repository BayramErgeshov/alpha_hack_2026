#pragma once

#include <string>

struct ParsedPage {
    std::string url;
    std::string title;
    std::string published_time;
    std::string content_markdown;
    std::string jina_markdown;
    std::string error;
    long http_status = 0;

    bool ok() const { return error.empty() && http_status >= 200 && http_status < 400; }
};

ParsedPage read_url(const std::string& url, long timeout_seconds = 25, bool jina_mode = true);

// Downloads a public http(s) resource as text (RSS/Atom feeds). Applies the same
// public-address checks and redirect validation as read_url().
bool fetch_public_text(const std::string& url, long timeout_seconds, std::string& body, std::string& error);
