#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#endif
// reader.cpp — mini-Reader: URL -> чистый Markdown для LLM
// Единственная зависимость: libcurl.
//
// Сборка (Linux/macOS): g++ -std=c++17 -O2 reader.cpp -o reader -lcurl
// Сборка (Windows/MSVC): cl /std:c++17 /O2 reader.cpp /link libcurl.lib
//
// Использование:
//   ./reader <url> [options]
//   -o, --output FILE     записать результат в файл
//   -t, --timeout SEC     таймаут запроса (по умолчанию 20)
//   -h, --help            справка

#include <curl/curl.h>
#include "reader.hpp"

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
// ==========================================================================
// Режим работы: raw (по умолчанию) или jina (как r.jina.ai)
// ==========================================================================
static thread_local bool g_jina_mode = false;
static thread_local int  g_image_counter = 0;

// ==========================================================================
// 1. DOM
// ==========================================================================
struct Node {
    bool text_node = false;
    std::string tag;                                   // для элементов
    std::string text;                                  // для текстовых узлов
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<Node>> children;
    Node* parent = nullptr;
    double score = 0.0;

    std::string attr(const std::string& k) const {
        for (auto& [n, v] : attrs) if (n == k) return v;
        return "";
    }
};

template <typename F>
void walk(Node* n, F&& cb) {
    cb(n);
    for (auto& c : n->children) walk(c.get(), cb);
}

void get_text_into(const Node* n, std::string& out) {
    if (n->text_node) { out += n->text; return; }
    for (auto& c : n->children) get_text_into(c.get(), out);
}
std::string get_text(const Node* n) {
    std::string s;
    get_text_into(n, s);
    return s;
}

void detach_node(Node* n) {
    if (!n->parent) return;
    auto& v = n->parent->children;
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->get() == n) { v.erase(it); return; }
    }
}

// ==========================================================================
// 2. Утилиты
// ==========================================================================
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string collapse_ws(const std::string& s) {
    std::string out;
    bool prev_space = false;
    for (char c : s) {
        if (std::isspace((unsigned char)c)) {
            if (!prev_space) out += ' ';
            prev_space = true;
        }
        else {
            out += c;
            prev_space = false;
        }
    }
    return out;
}

std::string decode_entities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        if (s[i] != '&') { out += s[i++]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 12) { out += s[i++]; continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp")  out += '&';
        else if (ent == "lt")   out += '<';
        else if (ent == "gt")   out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent == "nbsp") out += ' ';
        else if (ent.size() > 1 && ent[0] == '#') {
            long cp = 0;
            try {
                if (ent[1] == 'x' || ent[1] == 'X') cp = std::stol(ent.substr(2), nullptr, 16);
                else                                 cp = std::stol(ent.substr(1));
            }
            catch (...) { out += s[i++]; continue; }
            if (cp < 0x80)    out += char(cp);
            else if (cp < 0x800) { out += char(0xC0 | (cp >> 6));   out += char(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) {
                out += char(0xE0 | (cp >> 12));  out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            }
            else {
                out += char(0xF0 | (cp >> 18));  out += char(0x80 | ((cp >> 12) & 0x3F));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            }
        }
        else { out += s[i++]; continue; }
        i = semi + 1;
    }
    return out;
}

// ==========================================================================
// 3. HTML-парсер
// ==========================================================================
struct Tags {
    static const std::unordered_set<std::string>& voids() {
        static const std::unordered_set<std::string> s = {
            "area","base","br","col","embed","hr","img","input",
            "link","meta","param","source","track","wbr" };
        return s;
    }
    static const std::unordered_set<std::string>& parser_ignored() {
        static const std::unordered_set<std::string> s = {
            "script","style","noscript","template","iframe","svg","canvas" };
        return s;
    }
    static const std::unordered_set<std::string>& preclean_ignored() {
        static const std::unordered_set<std::string> s = {
            "script","style","noscript","template","iframe","svg","canvas",
            "nav","aside","footer","form","head","title","meta","link" };   
        return s;
    }
    static const std::unordered_map<std::string, std::unordered_set<std::string>>& auto_close() {
        static const std::unordered_map<std::string, std::unordered_set<std::string>> m = []() {
            std::unordered_map<std::string, std::unordered_set<std::string>> x;
            x["p"] = { "p" };
            x["li"] = { "li" };
            x["td"] = { "td", "th" };
            x["th"] = { "td", "th" };
            x["tr"] = { "tr", "td", "th" };
            x["dd"] = { "dt", "dd" };
            x["dt"] = { "dt", "dd" };
            for (const char* t : { "div","h1","h2","h3","h4","h5","h6","ul","ol",
                                  "table","blockquote","pre","section","article",
                                  "main","header","footer","nav","aside","hr",
                                  "figure","dl" }) {
                x[t].insert("p");
            }
            return x;
            }();
        return m;
    }
};

class HtmlParser {
public:
    std::unique_ptr<Node> root;
    std::vector<Node*> stack;
    int ignore_depth = 0;

    HtmlParser() {
        root = std::make_unique<Node>();
        root->tag = "root";
        stack.push_back(root.get());
    }

    void parse(const std::string& html) {
        size_t n = html.size();
        size_t i = 0, text_start = 0;
        while (i < n) {
            if (html[i] != '<') { ++i; continue; }
            if (i > text_start) emit_text(html.substr(text_start, i - text_start));

            // комментарий
            if (i + 3 < n && html.compare(i, 4, "<!--") == 0) {
                size_t e = html.find("-->", i + 4);
                i = (e == std::string::npos) ? n : e + 3;
                text_start = i;
                continue;
            }
            // CDATA
            if (i + 8 < n && html.compare(i, 9, "<![CDATA[") == 0) {
                size_t e = html.find("]]>", i + 9);
                if (e == std::string::npos) { i = n; break; }
                emit_text(html.substr(i + 9, e - i - 9));
                i = e + 3;
                text_start = i;
                continue;
            }
            // DOCTYPE / PI
            if (i + 1 < n && (html[i + 1] == '!' || html[i + 1] == '?')) {
                size_t e = html.find('>', i);
                i = (e == std::string::npos) ? n : e + 1;
                text_start = i;
                continue;
            }
            // закрывающий тег
            if (i + 1 < n && html[i + 1] == '/') {
                size_t e = html.find('>', i);
                if (e == std::string::npos) { i = n; break; }
                std::string name = html.substr(i + 2, e - i - 2);
                while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
                std::transform(name.begin(), name.end(), name.begin(),
                    [](unsigned char c) { return (char)std::tolower(c); });
                for (int k = (int)stack.size() - 1; k >= 1; --k) {
                    if (stack[k]->tag == name) {
                        if (Tags::parser_ignored().count(name) && ignore_depth > 0)
                            --ignore_depth;
                        stack.resize(k);
                        break;
                    }
                }
                i = e + 1;
                text_start = i;
                continue;
            }
            // открывающий тег
            size_t j = i + 1;
            while (j < n && !std::isspace((unsigned char)html[j]) &&
                html[j] != '>' && html[j] != '/') ++j;
            std::string name = html.substr(i + 1, j - i - 1);
            std::transform(name.begin(), name.end(), name.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });
            if (name.empty()) { i = j; continue; }

            std::vector<std::pair<std::string, std::string>> attrs;
            bool self_closing = false;
            while (j < n && html[j] != '>') {
                if (std::isspace((unsigned char)html[j])) { ++j; continue; }
                if (html[j] == '/') { self_closing = true; ++j; continue; }

                size_t an = j;
                while (j < n && !std::isspace((unsigned char)html[j]) &&
                    html[j] != '=' && html[j] != '>' && html[j] != '/') ++j;
                std::string aname = html.substr(an, j - an);
                std::transform(aname.begin(), aname.end(), aname.begin(),
                    [](unsigned char c) { return (char)std::tolower(c); });

                while (j < n && std::isspace((unsigned char)html[j])) ++j;
                std::string aval;
                if (j < n && html[j] == '=') {
                    ++j;
                    while (j < n && std::isspace((unsigned char)html[j])) ++j;
                    if (j < n && (html[j] == '"' || html[j] == '\'')) {
                        char q = html[j++];
                        size_t vs = j;
                        while (j < n && html[j] != q) ++j;
                        aval = html.substr(vs, j - vs);
                        if (j < n) ++j;
                    }
                    else {
                        size_t vs = j;
                        while (j < n && !std::isspace((unsigned char)html[j]) && html[j] != '>') ++j;
                        aval = html.substr(vs, j - vs);
                    }
                }
                attrs.emplace_back(std::move(aname), decode_entities(aval));
            }
            if (j < n && html[j] == '>') ++j;

            // автозакрытие
            auto& ac = Tags::auto_close();
            auto it = ac.find(name);
            if (it != ac.end() && !stack.empty() &&
                it->second.count(stack.back()->tag)) {
                stack.pop_back();
            }

            auto node = std::make_unique<Node>();
            node->tag = name;
            node->attrs = std::move(attrs);
            node->parent = stack.back();
            Node* raw = node.get();
            stack.back()->children.push_back(std::move(node));

            if (Tags::voids().count(name) || self_closing) {
                i = j; text_start = i; continue;
            }
            if (Tags::parser_ignored().count(name)) ++ignore_depth;
            stack.push_back(raw);
            i = j;
            text_start = i;
        }
        if (n > text_start) emit_text(html.substr(text_start, n - text_start));
    }

private:
    void emit_text(const std::string& txt) {
        if (ignore_depth > 0 || txt.empty()) return;
        auto node = std::make_unique<Node>();
        node->text_node = true;
        node->text = decode_entities(txt);
        node->parent = stack.back();
        stack.back()->children.push_back(std::move(node));
    }
};

// ==========================================================================
// 4. Извлечение основного контента (Readability-lite)
// ==========================================================================
struct Score {
    static const std::unordered_set<std::string>& block_tags() {
        static const std::unordered_set<std::string> s = {
            "p","div","article","section","main","td","pre","blockquote",
            "li","h1","h2","h3","h4","h5","h6" };
        return s;
    }
    static const std::unordered_map<std::string, double>& tag_weight() {
        static const std::unordered_map<std::string, double> m = {
            { "article", 5 },{ "main", 5 },{ "div", 3 },{ "section", 2 },
            { "pre", 3 },{ "td", 3 },{ "blockquote", 3 },{ "p", 1 } };
        return m;
    }
    static const std::regex& unlikely_re() {
        static const std::regex r(
            "comment|sidebar|footer|header|nav|menu|advert|banner|popup|modal|"
            "share|social|related|recommend|breadcrumb|pagination|widget|cookie",
            std::regex_constants::icase);
        return r;
    }
    static const std::regex& positive_re() {
        static const std::regex r("article|content|main|post|entry|story|body|text|blog",
            std::regex_constants::icase);
        return r;
    }
    static const std::regex& negative_re() {
        static const std::regex r(
            "comment|sidebar|footer|header|nav|menu|advert|banner|share|social|related",
            std::regex_constants::icase);
        return r;
    }
};

void preclean(Node* root) {
    std::vector<Node*> to_kill;
    std::function<void(Node*)> visit = [&](Node* n) {
        if (n->text_node) return;
        if (Tags::preclean_ignored().count(n->tag)) { to_kill.push_back(n); return; }

        // --- Jina mode: Wikipedia-specific cleaning ---
        if (g_jina_mode) {
            std::string cls = n->attr("class");
            std::string id = n->attr("id");
            if (cls.find("mw-editsection") != std::string::npos ||
                cls.find("mw-redirectedfrom") != std::string::npos ||
                cls.find("mw-jump-link") != std::string::npos ||
                cls.find("ambox") != std::string::npos ||
                cls.find("hatnote") != std::string::npos ||
                cls.find("navbox") != std::string::npos ||
                id == "siteSub" ||
                id == "contentSub" ||
                id == "jump-to-nav" ||
                id == "toc") {
                to_kill.push_back(n);
                return;
            }
        }

        std::string ident = n->attr("id") + " " + n->attr("class");
        // Не вырезать семантические контейнеры (они всегда полезны)
        bool is_semantic = (n->tag == "main" || n->tag == "article");
        if (!ident.empty() && !is_semantic && n->tag != "a" &&
            n->tag != "body" && n->tag != "html" &&
            std::regex_search(ident, Score::unlikely_re()) &&
            !std::regex_search(ident, Score::positive_re())) {
            to_kill.push_back(n);
            return;
        }
        for (auto& c : n->children) visit(c.get());
        };
    visit(root);
#ifdef _DEBUG
    std::cerr << "[preclean] removing " << to_kill.size() << " nodes\n";
#endif
    for (Node* n : to_kill) detach_node(n);
}

void compute_scores(Node* root) {
    walk(root, [](Node* n) {
        if (n->text_node || !Score::block_tags().count(n->tag)) return;
        std::string txt = trim(get_text(n));
        if (txt.size() < 25) return;

        int commas = 0;
        for (char c : txt) if (c == ',' || c == '.') ++commas;
        double base = 1.0 + commas + std::min(txt.size() / 100.0, 3.0);

        size_t link_chars = 0;
        walk(n, [&](Node* d) {
            if (!d->text_node && d->tag == "a") link_chars += get_text(d).size();
            });
        double ld = double(link_chars) / std::max<size_t>(txt.size(), 1);
        if (ld > 0.5) base *= (1.0 - ld);

        std::string ident = n->attr("id") + " " + n->attr("class");
        if (std::regex_search(ident, Score::positive_re())) base *= 1.5;
        if (std::regex_search(ident, Score::negative_re())) base *= 0.5;

        auto tw = Score::tag_weight().find(n->tag);
        if (tw != Score::tag_weight().end()) base *= tw->second;

        n->score += base;
        Node* p = n->parent;
        int depth = 1;
        while (p && p->tag != "root" && depth <= 5) {
            p->score += base * std::pow(0.5, depth);
            p = p->parent;
            ++depth;
        }
        });
}

std::unique_ptr<Node> extract_main(std::unique_ptr<Node> root_holder) {
    Node* root = root_holder.get();
    preclean(root);
    compute_scores(root);

    Node* best = root;
    walk(root, [&](Node* n) {
        if (!n->text_node && n->score > best->score) best = n;
        });
    if (best == root || best->score <= 0.0) return root_holder;
#ifdef _DEBUG
    std::cerr << "[extract] best tag=" << best->tag
        << " class=\"" << best->attr("class").substr(0, 60)
        << "\" score=" << best->score << "\n";
#endif
    while (best->parent && best->parent->tag != "root" &&
        best->parent->score >= best->score * 0.8) {
        best = best->parent;
    }

    double threshold = best->score * 0.2;
    auto result = std::make_unique<Node>();
    result->tag = "div";

    auto& sibs = (best->parent && best->parent->tag != "root")
        ? best->parent->children
        : root->children;

    for (auto it = sibs.begin(); it != sibs.end(); ) {
        Node* child = it->get();
        if (!child->text_node && (child == best || child->score >= threshold)) {
            child->parent = result.get();
            result->children.push_back(std::move(*it));
            it = sibs.erase(it);
        }
        else {
            ++it;
        }
    }
    std::cerr << "[extract] result children: " << result->children.size() << "\n";
    return result;
}

// ==========================================================================
// 5. HTML -> Markdown
// ==========================================================================
std::string join_url(const std::string& base, const std::string& href) {
    if (href.empty() || base.empty()) return href;
    if (href.find("://") != std::string::npos) return href;
    CURLU* u = curl_url();
    if (!u) return href;
    std::string out = href;
    if (curl_url_set(u, CURLUPART_URL, base.c_str(), 0) == CURLUE_OK &&
        curl_url_set(u, CURLUPART_URL, href.c_str(), 0) == CURLUE_OK) {
        char* res = nullptr;
        if (curl_url_get(u, CURLUPART_URL, &res, 0) == CURLUE_OK) {
            out = res;
            curl_free(res);
        }
    }
    curl_url_cleanup(u);
    return out;
}

std::string encode_nonascii_in_url(const std::string& url) {
    std::string out;
    out.reserve(url.size() * 2);
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : url) {
        if (c < 128) {
            out += (char)c;
        }
        else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

void inline_walk(const Node* n, std::string& out, const std::string& base) {
    if (n->text_node) { out += collapse_ws(n->text); return; }

    const std::string& t = n->tag;
    if (t == "strong" || t == "b") {
        out += "**"; for (auto& c : n->children) inline_walk(c.get(), out, base); out += "**";
    }
    else if (t == "em" || t == "i") {
        out += "*"; for (auto& c : n->children) inline_walk(c.get(), out, base); out += "*";
    }
    else if (t == "del" || t == "s" || t == "strike") {
        out += "~~"; for (auto& c : n->children) inline_walk(c.get(), out, base); out += "~~";
    }
    else if (t == "code") {
        out += "`"; for (auto& c : n->children) inline_walk(c.get(), out, base); out += "`";
    }
    else if (t == "a") {
        std::string href_raw = n->attr("href");

        // Jina mode: skip edit links (Wikipedia)
        if (g_jina_mode &&
            (href_raw.find("action=edit") != std::string::npos ||
                href_raw.find("veaction=edit") != std::string::npos)) {
            return;
        }

        std::string href = join_url(base, href_raw);
        std::string title = n->attr("title");
        if (g_jina_mode) href = encode_nonascii_in_url(href);

        out += "[";
        for (auto& c : n->children) inline_walk(c.get(), out, base);
        out += "](" + href;
        if (g_jina_mode && !title.empty()) {
            std::string esc;
            for (char ch : title) { if (ch == '"') esc += "\\\""; else esc += ch; }
            out += " \"" + esc + "\"";
        }
        out += ")";
    }
    else if (t == "img") {
        std::string src = join_url(base, n->attr("src"));
        std::string alt = n->attr("alt");
        if (g_jina_mode) {
            src = encode_nonascii_in_url(src);
            if (alt.empty() || alt.size() < 3) {
                alt = "Image " + std::to_string(++g_image_counter);
            }
        }
        out += "![" + alt + "](" + src + ")";
    }
    else if (t == "br") {
        out += "\n";
    }
    else {
        for (auto& c : n->children) inline_walk(c.get(), out, base);
    }
}

std::string render_inline(const Node* n, const std::string& base) {
    std::string s;
    inline_walk(n, s, base);
    return s;
}

void collect_raw_text(const Node* n, std::string& out) {
    if (n->text_node) { out += n->text; return; }
    for (auto& c : n->children) collect_raw_text(c.get(), out);
}

void collect_rows(const Node* n,
    std::vector<std::vector<std::string>>& rows,
    const std::string& base) {
    if (n->text_node) return;
    if (n->tag == "tr") {
        std::vector<std::string> row;
        for (auto& c : n->children) {
            if (c->text_node) continue;
            if (c->tag == "td" || c->tag == "th")
                row.push_back(trim(render_inline(c.get(), base)));
        }
        if (!row.empty()) rows.push_back(std::move(row));
        return;
    }
    for (auto& c : n->children) collect_rows(c.get(), rows, base);
}

std::string render_table(const Node* n, const std::string& base) {
    std::vector<std::vector<std::string>> rows;
    collect_rows(n, rows, base);
    if (rows.empty()) return "";
    size_t width = 0;
    for (auto& r : rows) width = std::max(width, r.size());

    std::string out;
    for (size_t i = 0; i < rows.size(); ++i) {
        auto& r = rows[i];
        out += "| ";
        for (size_t j = 0; j < width; ++j) {
            out += (j < r.size() ? r[j] : "");
            if (j + 1 < width) out += " | ";
        }
        out += " |\n";
        if (i == 0) {
            out += "|";
            for (size_t j = 0; j < width; ++j) out += " --- |";
            out += "\n";
        }
    }
    return out;
}

void render_block(const Node* n, std::string& out, const std::string& base) {
    if (n->text_node) {
        std::string s = trim(collapse_ws(n->text));
        if (!s.empty()) { out += s; out += "\n\n"; }
        return;
    }
    const std::string& t = n->tag;

    if (t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6') {
        out.append(t[1] - '0', '#');
        out += ' ';
        out += trim(render_inline(n, base));
        out += "\n\n";
    }
    else if (t == "p") {
        std::string s = trim(render_inline(n, base));
        if (!s.empty()) { out += s; out += "\n\n"; }
    }
    else if (t == "div" || t == "section" || t == "article" || t == "main" ||
        t == "aside" || t == "header" || t == "footer" || t == "nav" ||
        t == "body" || t == "html" || t == "root") {
        for (auto& c : n->children) render_block(c.get(), out, base);
    }
    else if (t == "ul" || t == "ol") {
        int idx = 1;
        for (auto& c : n->children) {
            if (c->text_node || c->tag != "li") continue;
            std::string prefix = (t == "ul") ? "- " : (std::to_string(idx++) + ". ");
            out += prefix + trim(render_inline(c.get(), base)) + "\n";
        }
        out += "\n";
    }
    else if (t == "blockquote") {
        std::string s = trim(render_inline(n, base));
        std::stringstream ss(s);
        std::string line;
        while (std::getline(ss, line)) out += "> " + line + "\n";
        out += "\n";
    }
    else if (t == "pre") {
        std::string s;
        collect_raw_text(n, s);
        out += "```\n" + s + "\n```\n\n";
    }
    else if (t == "hr") {
        out += "---\n\n";
    }
    else if (t == "table") {
        out += render_table(n, base) + "\n\n";
    }
    else if (t == "br") {
        out += "\n";
    }
    else {
        std::string s = trim(render_inline(n, base));
        if (!s.empty()) { out += s; out += "\n\n"; }
    }
}

// ==========================================================================
// Метаданные страницы (для jina mode)
// ==========================================================================
std::string find_title_text(Node* root) {
    std::string title;
    walk(root, [&](Node* n) {
        if (n->text_node || !title.empty()) return;
        if (n->tag == "title") title = trim(get_text(n));
        });
    // Отрезать " — Wikipedia", " | Site", " - Site" если хвост короткий
    if (g_jina_mode && !title.empty()) {
        for (const char* sep : { " — ", " | ", " - " }) {
            size_t pos = title.find(sep);
            if (pos != std::string::npos && pos > 3) {
                std::string suffix = title.substr(pos + std::strlen(sep));
                if (suffix.size() < 40) { title = trim(title.substr(0, pos)); break; }
            }
        }
    }
    return title;
}

std::string find_meta_content(Node* root, const std::string& key) {
    std::string result;
    walk(root, [&](Node* n) {
        if (n->text_node || !result.empty()) return;
        if (n->tag != "meta") return;
        if (n->attr("property") == key || n->attr("name") == key)
            result = n->attr("content");
        });
    return result;
}

std::string to_markdown(const Node* n, const std::string& base) {
    std::string out;
    render_block(n, out, base);
    // схлопываем 3+ перевода строки
    std::string collapsed;
    collapsed.reserve(out.size());
    int nl = 0;
    for (char c : out) {
        if (c == '\n') { ++nl; if (nl <= 2) collapsed += c; }
        else { nl = 0; collapsed += c; }
    }
    return trim(collapsed) + "\n";
}
static thread_local long g_last_http_status = 0;
static thread_local std::string g_last_effective_url;
static thread_local std::string g_last_content_type;
static thread_local std::string g_last_redirect_location;
static thread_local std::string g_last_fetch_error;
// ==========================================================================
// 6. HTTP через libcurl
// ==========================================================================
static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    const size_t bytes = size * nmemb;
    constexpr size_t kMaxHtmlBytes = 12 * 1024 * 1024;
    if (bytes > kMaxHtmlBytes - std::min(body->size(), kMaxHtmlBytes)) return 0;
    body->append(ptr, bytes);
    return bytes;
}

std::string fetch_html(const std::string& url, long timeout) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    timeout = std::clamp(timeout, 1L, 60L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min(timeout, 12L));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8");
    hdrs = curl_slist_append(hdrs, "Accept-Language: en-US,en;q=0.9,ru;q=0.8");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Dest: document");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Mode: navigate");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Site: none");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-User: ?1");
    hdrs = curl_slist_append(hdrs, "Upgrade-Insecure-Requests: 1");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    g_last_redirect_location.clear();
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
        +[](char* data, size_t size, size_t count, void* userdata) -> size_t {
            const size_t bytes = size * count;
            std::string line(data, bytes);
            std::transform(line.begin(), line.end(), line.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (line.rfind("location:", 0) == 0) {
                auto* location = static_cast<std::string*>(userdata);
                *location = trim(std::string(data + 9, bytes > 9 ? bytes - 9 : 0));
            }
            return bytes;
        });
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &g_last_redirect_location);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    char* ctype = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype);
    char* effective_url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    g_last_http_status = status;
    g_last_effective_url = effective_url ? effective_url : url;
    g_last_content_type = ctype ? ctype : "";
    g_last_fetch_error.clear();

#ifdef _DEBUG
    std::cerr << "[debug] HTTP " << status
        << ", Content-Type: " << (ctype ? ctype : "?")
        << ", HTML: " << body.size() << " bytes\n";
#endif

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        g_last_fetch_error = curl_easy_strerror(rc);
        std::cerr << "fetch error: " << curl_easy_strerror(rc) << "\n";
        return "";
    }
    if (status >= 400) std::cerr << "HTTP status " << status << "\n";
    return body;
}

// ==========================================================================
// 7. URL safety and public reader API
static bool private_ipv4(const unsigned char* a) {
    return a[0] == 0 || a[0] == 10 || a[0] == 127 || a[0] >= 224 ||
        (a[0] == 169 && a[1] == 254) ||
        (a[0] == 172 && (a[1] >= 16 && a[1] <= 31)) ||
        (a[0] == 192 && a[1] == 168) ||
        (a[0] == 100 && (a[1] >= 64 && a[1] <= 127)) ||
        (a[0] == 192 && a[1] == 0 && a[2] == 0) ||
        (a[0] == 192 && a[1] == 0 && a[2] == 2) ||
        (a[0] == 198 && (a[1] == 18 || a[1] == 19)) ||
        (a[0] == 198 && a[1] == 51 && a[2] == 100) ||
        (a[0] == 203 && a[1] == 0 && a[2] == 113);
}

static bool private_ipv6(const unsigned char* a) {
    const bool loopback = std::all_of(a, a + 15, [](unsigned char b) { return b == 0; }) && a[15] == 1;
    if (loopback || std::all_of(a, a + 16, [](unsigned char b) { return b == 0; })) return true;
    if ((a[0] & 0xFE) == 0xFC || (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) || a[0] == 0xFF) return true;
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x0D && a[3] == 0xB8) return true;
    static const unsigned char mapped_prefix[12] = { 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF };
    if (std::equal(mapped_prefix, mapped_prefix + 12, a)) return private_ipv4(a + 12);
    return false;
}

static bool private_host(const std::string& source_host) {
    std::string host = source_host;
    std::transform(host.begin(), host.end(), host.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    if (host.empty() || host == "localhost" || host == "metadata.google.internal" ||
        host.ends_with(".localhost") || host.ends_with(".local") || host.ends_with(".internal")) return true;

    unsigned char ipv4[4]{};
    unsigned char ipv6[16]{};
#ifdef _WIN32
    if (InetPtonA(AF_INET, host.c_str(), ipv4) == 1) return private_ipv4(ipv4);
    if (InetPtonA(AF_INET6, host.c_str(), ipv6) == 1) return private_ipv6(ipv6);
#else
    if (inet_pton(AF_INET, host.c_str(), ipv4) == 1) return private_ipv4(ipv4);
    if (inet_pton(AF_INET6, host.c_str(), ipv6) == 1) return private_ipv6(ipv6);
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &addresses) != 0) return false;
    bool is_private = false;
    for (addrinfo* item = addresses; item && !is_private; item = item->ai_next) {
        if (item->ai_family == AF_INET) {
            auto* address = reinterpret_cast<sockaddr_in*>(item->ai_addr);
            auto* bytes = reinterpret_cast<unsigned char*>(&address->sin_addr);
            is_private = private_ipv4(bytes);
        } else if (item->ai_family == AF_INET6) {
            auto* address = reinterpret_cast<sockaddr_in6*>(item->ai_addr);
            auto* bytes = reinterpret_cast<unsigned char*>(&address->sin6_addr);
            is_private = private_ipv6(bytes);
        }
    }
    freeaddrinfo(addresses);
    return is_private;
}

static bool safe_http_url(const std::string& source, std::string& error) {
    if (source.empty() || source.size() > 4096) {
        error = "URL is empty or exceeds 4096 characters";
        return false;
    }
    CURLU* parsed = curl_url();
    if (!parsed) { error = "URL parser is unavailable"; return false; }
    CURLUcode rc = curl_url_set(parsed, CURLUPART_URL, source.c_str(), 0);
    char* scheme_value = nullptr;
    char* host_value = nullptr;
    char* user_value = nullptr;
    if (rc != CURLUE_OK || curl_url_get(parsed, CURLUPART_SCHEME, &scheme_value, 0) != CURLUE_OK ||
        curl_url_get(parsed, CURLUPART_HOST, &host_value, 0) != CURLUE_OK) {
        error = "URL must be an absolute http or https URL";
        curl_url_cleanup(parsed);
        return false;
    }
    std::string scheme = scheme_value ? scheme_value : "";
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool has_userinfo = curl_url_get(parsed, CURLUPART_USER, &user_value, 0) == CURLUE_OK;
    bool allowed = (scheme == "http" || scheme == "https") && !has_userinfo &&
        !private_host(host_value ? host_value : "");
    curl_free(scheme_value);
    curl_free(host_value);
    if (user_value) curl_free(user_value);
    curl_url_cleanup(parsed);
    if (!allowed) error = "Only public http/https URLs without credentials are allowed";
    return allowed;
}

ParsedPage read_url(const std::string& url, long timeout_seconds, bool jina_mode) {
    ParsedPage page;
    page.url = url;
    std::string validation_error;
    if (!safe_http_url(url, validation_error)) {
        page.error = validation_error;
        return page;
    }

    g_jina_mode = jina_mode;
    g_image_counter = 0;
    g_last_fetch_error.clear();
    std::string current_url = url;
    std::string html;
    bool redirected = false;
    for (int hop = 0; hop <= 5; ++hop) {
        if (!safe_http_url(current_url, validation_error)) {
            page.error = "Redirect rejected: " + validation_error;
            return page;
        }
        html = fetch_html(current_url, timeout_seconds);
        page.http_status = g_last_http_status;
        if (page.http_status >= 300 && page.http_status < 400 && !g_last_redirect_location.empty()) {
            if (hop == 5) { page.error = "Too many redirects"; return page; }
            current_url = join_url(current_url, g_last_redirect_location);
            redirected = true;
            continue;
        }
        break;
    }

    if (page.http_status < 200 || page.http_status >= 300) {
        page.error = page.http_status ? "Source returned HTTP " + std::to_string(page.http_status) :
            (g_last_fetch_error.empty() ? "Unable to fetch source" : g_last_fetch_error);
        return page;
    }
    if (html.empty()) {
        page.error = g_last_fetch_error.empty() ? "Source returned an empty response" : g_last_fetch_error;
        return page;
    }
    if (g_last_content_type.find("application/pdf") != std::string::npos ||
        g_last_content_type.find("image/") == 0 ||
        g_last_content_type.find("application/octet-stream") != std::string::npos) {
        page.error = "The source is not an HTML or text page";
        return page;
    }

    HtmlParser parser;
    parser.parse(html);
    page.title = find_title_text(parser.root.get());
    if (page.title.empty()) page.title = find_meta_content(parser.root.get(), "og:title");
    if (page.title.empty()) page.title = find_meta_content(parser.root.get(), "twitter:title");
    page.published_time = find_meta_content(parser.root.get(), "article:published_time");
    if (page.published_time.empty()) page.published_time = find_meta_content(parser.root.get(), "og:published_time");
    if (page.published_time.empty()) page.published_time = find_meta_content(parser.root.get(), "datePublished");
    if (page.published_time.empty()) page.published_time = find_meta_content(parser.root.get(), "date");

    auto main_node = extract_main(std::move(parser.root));
    page.content_markdown = to_markdown(main_node.get(), redirected ? current_url : url);
    if (jina_mode) {
        if (!page.title.empty()) page.jina_markdown += "Title: " + page.title + "\n\n";
        page.jina_markdown += "URL Source: " + url + "\n\n";
        if (!page.published_time.empty()) page.jina_markdown += "Published Time: " + page.published_time + "\n\n";
        page.jina_markdown += "Markdown Content:\n" + page.content_markdown;
    } else {
        page.jina_markdown = page.content_markdown;
    }
    if (page.content_markdown == "\n") page.content_markdown.clear();
    if (page.content_markdown.empty()) page.error = "No readable content was found";
    return page;
}

bool fetch_public_text(const std::string& url, long timeout_seconds, std::string& body, std::string& error) {
    body.clear();
    std::string current = url;
    for (int hop = 0; hop <= 5; ++hop) {
        if (!safe_http_url(current, error)) { error = hop ? "Redirect rejected: " + error : error; return false; }
        g_last_fetch_error.clear();
        body = fetch_html(current, timeout_seconds);
        if (g_last_http_status >= 300 && g_last_http_status < 400 && !g_last_redirect_location.empty()) {
            current = join_url(current, g_last_redirect_location);
            continue;
        }
        if (g_last_http_status < 200 || g_last_http_status >= 300) {
            error = g_last_http_status ? "Source returned HTTP " + std::to_string(g_last_http_status)
                                       : (g_last_fetch_error.empty() ? "Unable to fetch source" : g_last_fetch_error);
            body.clear();
            return false;
        }
        if (body.empty()) { error = "Source returned an empty response"; return false; }
        return true;
    }
    error = "Too many redirects";
    body.clear();
    return false;
}
