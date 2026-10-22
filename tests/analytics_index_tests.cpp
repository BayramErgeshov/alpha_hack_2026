#include "analytics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

class Checks {
public:
    void expect(bool condition, const std::string& description) {
        ++total_;
        if (!condition) {
            ++failed_;
            std::cerr << "FAIL: " << description << '\n';
        }
    }

    int finish() const {
        std::cout << (total_ - failed_) << '/' << total_ << " checks passed\n";
        return failed_ == 0 ? 0 : 1;
    }

private:
    int total_ = 0;
    int failed_ = 0;
};

std::string path_utf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

// Keep temporary fixtures alongside this source, including when the executable
// runs from a build directory. Compile this file using its absolute path.
class FixtureDirectory {
public:
    FixtureDirectory() {
        base_ = fs::absolute(fs::path(__FILE__)).parent_path().lexically_normal();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = base_ / (".analytics_index_fixture_" + std::to_string(stamp));
        if (!fs::create_directory(root_)) throw std::runtime_error("Could not create fixture directory");
    }

    ~FixtureDirectory() {
        // Delete only this instance's generated child of the known tests folder.
        if (root_.parent_path() == base_ && root_.filename().string().find(".analytics_index_fixture_") == 0) {
            std::error_code ignored;
            fs::remove_all(root_, ignored);
        }
    }

    fs::path child(const std::string& name) const {
        const fs::path result = root_ / name;
        fs::create_directory(result);
        return result;
    }

private:
    fs::path base_;
    fs::path root_;
};

void write_text(const fs::path& filename, const std::string& text) {
    std::ofstream stream(filename, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not create test fixture");
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) throw std::runtime_error("Could not write test fixture");
}

bool valid_utf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        size_t length = 0;
        uint32_t point = 0;
        uint32_t minimum = 0;
        if (first < 0x80) { length = 1; point = first; }
        else if (first >= 0xC2 && first <= 0xDF) { length = 2; point = first & 0x1F; minimum = 0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { length = 3; point = first & 0x0F; minimum = 0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { length = 4; point = first & 0x07; minimum = 0x10000; }
        else return false;
        if (i + length > text.size()) return false;
        for (size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(text[i + offset]);
            if ((next & 0xC0) != 0x80) return false;
            point = (point << 6) | (next & 0x3F);
        }
        if (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return false;
        i += length;
    }
    return true;
}

void page_and_search_regressions(Checks& checks, const FixtureDirectory& fixture) {
    const fs::path directory = fixture.child("pages");
    const std::string unicode_filename = "Ставка ЦБ.md";
    write_text(directory / fs::u8path(unicode_filename),
        "# Original bank document\n"
        "<!-- extraction: windows_ocr -->\n"
        "preamblemustnotindex\n"
        "[[PAGE: 7]]\n"
        "Ключевая ставка снижена. Ключевая ставка влияет на стоимость кредита.\n"
        "[[PAGE: 8]]\n"
        "<!-- TEXT_UNAVAILABLE -->\n"
        "[[PAGE: 9]]\n"
        "Инфляция замедляется. Банк оценивает потребительский спрос.\n");
    write_text(directory / "metals.txt", "Металлургия: экспорт стали и производство проката.\n");
    write_text(directory / "legacy.txt", "OCR_REQUIRED: legacyplaceholder inflation data unavailable\n");

    AnalyticsIndex index;
    std::string error;
    checks.expect(index.load(path_utf8(directory), error), "synthetic corpus loads");
    checks.expect(error.empty(), "successful load has no diagnostic");
    checks.expect(index.document_count() == 2, "OCR_REQUIRED placeholder is excluded from document count");
    checks.expect(index.chunk_count() == 3, "preamble and TEXT_UNAVAILABLE page produce no chunks");

    const auto rates = index.search("КЛЮЧЕВАЯ СТАВКА", 5);
    checks.expect(!rates.empty(), "uppercase Russian BM25 query finds lowercase terms");
    if (!rates.empty()) {
        const auto& hit = rates.front();
        checks.expect(hit.document == unicode_filename, "Unicode source filename remains UTF-8");
        checks.expect(hit.id.find(unicode_filename + "#page=7#part=") == 0, "source identifier contains UTF-8 filename and original page");
        checks.expect(hit.page == 7, "first marked page retains original number");
        checks.expect(hit.score > 0, "Russian BM25 match has positive score");
        checks.expect(hit.extraction_method == "windows_ocr", "document extraction method reaches search hits");
        checks.expect(hit.text.find("preamblemustnotindex") == std::string::npos, "returned text excludes document preamble");
        checks.expect(hit.text.find("[[PAGE:") == std::string::npos, "returned text excludes page markers");
        checks.expect(valid_utf8(hit.document) && valid_utf8(hit.id) && valid_utf8(hit.text), "Russian hit strings are valid UTF-8");
    }

    const auto inflation = index.search("инфляция", 5);
    checks.expect(inflation.size() == 1 && inflation.front().page == 9, "later source page is not renumbered after an unavailable page");
    if (!inflation.empty()) checks.expect(inflation.front().extraction_method == "windows_ocr", "extraction method applies to later source pages");
    const auto metals = index.search("МЕТАЛЛУРГИЯ", 5);
    checks.expect(metals.size() == 1 && metals.front().document == "metals.txt" && metals.front().page == 1,
                  "unmarked plain text remains searchable as page one");
    checks.expect(index.search("preamblemustnotindex").empty(), "document preamble is not searchable");
    checks.expect(index.search("TEXT_UNAVAILABLE").empty(), "unavailable-page sentinel is not searchable");
    checks.expect(index.search("legacyplaceholder").empty(), "legacy OCR_REQUIRED placeholder is not searchable");
    checks.expect(index.search("ставка", 0).empty(), "zero result limit returns no hits");
    checks.expect(index.search("и в на для").empty(), "Russian stop-word-only query returns no hits");

    checks.expect(index.load(path_utf8(directory / "not_present"), error), "missing optional analytics directory is tolerated");
    checks.expect(index.document_count() == 0 && index.chunk_count() == 0 && index.search("ставка").empty(),
                  "reloading an empty corpus clears previous documents and postings");
}

void utf8_chunk_regressions(Checks& checks, const FixtureDirectory& fixture) {
    const fs::path directory = fixture.child("utf8_boundaries");
    // The first four-byte emoji starts at byte 4198; a naive 4200-byte
    // truncation cuts it in half. Every resulting chunk also contains 'ставка'.
    const std::string text = "ставка " + std::string(4185, 'a') + "📈 ставка "
                           + std::string(4185, 'b') + "📈 ставка\n";
    write_text(directory / "boundaries.txt", text);
    AnalyticsIndex index;
    std::string error;
    checks.expect(index.load(path_utf8(directory), error), "UTF-8 boundary corpus loads");
    checks.expect(index.chunk_count() >= 3, "long text is split into multiple chunks");
    const auto hits = index.search("ставка", 100);
    checks.expect(hits.size() == index.chunk_count(), "all boundary chunks remain retrievable");
    std::string joined;
    std::vector<AnalyticsHit> ordered = hits;
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        const auto part = [](const std::string& id) { return std::stoul(id.substr(id.rfind("#part=") + 6)); };
        return part(left.id) < part(right.id);
    });
    for (const auto& hit : ordered) {
        checks.expect(valid_utf8(hit.text), "chunk " + hit.id + " does not split UTF-8 codepoints");
        joined += hit.text;
    }
    checks.expect(joined.find("📈") != std::string::npos && valid_utf8(joined), "chunked text preserves complete four-byte characters");
}

void real_corpus_checks(Checks& checks, const std::string& path) {
    AnalyticsIndex index;
    std::string error;
    checks.expect(index.load(path, error), "real analytics corpus loads: " + error);
    checks.expect(index.document_count() == 9, "real corpus contains nine bank documents");
    checks.expect(index.chunk_count() >= 64, "real corpus contains at least 64 text chunks");
    const auto hits = index.search("ключевая ставка инфляция", 10);
    checks.expect(!hits.empty(), "real bank-rate query returns evidence");
    for (const auto& hit : hits) {
        checks.expect(hit.page > 0 && !hit.text.empty() && hit.score > 0, "real hit carries source page and scored evidence");
        checks.expect(valid_utf8(hit.document) && valid_utf8(hit.id) && valid_utf8(hit.text), "real hit preserves Unicode source and text");
        checks.expect(hit.text.find("TEXT_UNAVAILABLE") == std::string::npos && hit.text.find("OCR_REQUIRED:") == std::string::npos,
                      "real hit excludes unavailable-text placeholders");
    }
}

} // namespace

int main(int argc, char** argv) {
    Checks checks;
    try {
        FixtureDirectory fixture;
        page_and_search_regressions(checks, fixture);
        utf8_chunk_regressions(checks, fixture);
        if (argc > 1) real_corpus_checks(checks, argv[1]);
    } catch (const std::exception& error) {
        checks.expect(false, std::string("unexpected exception: ") + error.what());
    }
    return checks.finish();
}
