// Native, local image OCR helper for preparing the analytical document corpus.
// Requires the Windows 10 SDK C++/WinRT headers and windowsapp.lib.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace winrt;
using namespace Windows::Globalization;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media::Ocr;
using namespace Windows::Storage;

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        init_apartment(apartment_type::multi_threaded);
        if (argc == 2 && std::wstring_view(argv[1]) == L"--languages") {
            for (const auto& language : OcrEngine::AvailableRecognizerLanguages()) {
                std::cout << to_string(language.LanguageTag()) << '\n';
            }
            return 0;
        }
        if (argc < 3 || argc > 4) {
            std::cerr << "Usage: AlphaPdfOcr.exe INPUT_PNG OUTPUT_TXT [language=ru]\n"
                         "       AlphaPdfOcr.exe --languages\n";
            return 2;
        }
        const fs::path input = fs::absolute(argv[1]);
        const fs::path output = fs::absolute(argv[2]);
        if (!fs::is_regular_file(input)) {
            std::cerr << "Input image does not exist.\n";
            return 3;
        }
        const std::wstring requested = argc == 4 ? argv[3] : L"ru";
        auto engine = OcrEngine::TryCreateFromLanguage(Language(requested));
        if (!engine && requested != L"en-US") {
            std::cerr << "Requested OCR language is unavailable; trying en-US.\n";
            engine = OcrEngine::TryCreateFromLanguage(Language(L"en-US"));
        }
        if (!engine) {
            std::cerr << "No requested or English OCR language is installed. "
                         "Install a Windows OCR language pack.\n";
            return 4;
        }
        auto file = StorageFile::GetFileFromPathAsync(input.wstring()).get();
        auto stream = file.OpenAsync(FileAccessMode::Read).get();
        auto decoder = BitmapDecoder::CreateAsync(stream).get();
        const auto maximum = OcrEngine::MaxImageDimension();
        if (!decoder.PixelWidth() || !decoder.PixelHeight() ||
            decoder.PixelWidth() > maximum || decoder.PixelHeight() > maximum) {
            std::cerr << "Image dimensions must be between 1 and " << maximum
                      << " pixels per side; got " << decoder.PixelWidth() << 'x'
                      << decoder.PixelHeight() << ". Resize the rendered page.\n";
            return 5;
        }
        auto bitmap = decoder.GetSoftwareBitmapAsync(
            BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied).get();
        auto result = engine.RecognizeAsync(bitmap).get();
        std::ofstream destination(output, std::ios::binary | std::ios::trunc);
        if (!destination) {
            std::cerr << "Cannot open output text file.\n";
            return 6;
        }
        std::size_t characters = 0;
        std::size_t lines = 0;
        for (const auto& line : result.Lines()) {
            const auto text = to_string(line.Text());
            destination << text << '\n';
            characters += text.size();
            ++lines;
        }
        destination.flush();
        if (!destination) {
            std::cerr << "Failed to write OCR output.\n";
            return 7;
        }
        std::cerr << "OCR language=" << to_string(engine.RecognizerLanguage().LanguageTag())
                  << " lines=" << lines << " utf8_bytes=" << characters << '\n';
        return 0;
    } catch (const hresult_error& error) {
        std::cerr << "Windows OCR failed (0x" << std::hex
                  << static_cast<unsigned long>(error.code()) << std::dec
                  << "): " << to_string(error.message()) << '\n';
        return 8;
    } catch (const std::exception& error) {
        std::cerr << "OCR failed: " << error.what() << '\n';
        return 9;
    }
}
