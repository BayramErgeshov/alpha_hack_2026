# Third-party components

- **libcurl** is used for HTTP(S) page retrieval and the OpenAI-compatible model gateway. It is linked as an external dependency and is not vendored in this source tree. See the libcurl license and notices distributed with the installed package.
- The HTML tree parser and readability/Markdown conversion in `src/reader.cpp` are based on the parser source already present in the supplied workspace and were adapted for the backend interface, request limits, and public-URL checks.
- **pypdfium2 / PDFium** and **Pillow** are used by the local data preparation script to read PDF pages and render image pages. Their license notices are distributed with the respective Python packages.
- **Windows.Media.Ocr** is a Windows system API used by the C++ OCR helper. The helper performs recognition locally and does not redistribute Windows components.

