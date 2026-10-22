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
