# Third-party components, models, datasets, services and simulated integrations

## Software
- **libcurl** (curl license, MIT-like) — HTTP(S) for news pages, RSS feeds and the model gateway. Linked dynamically; not vendored. Build-time headers come from the system package (`libcurl4-openssl-dev`).
- **HTML parser and readability/Markdown conversion** in `src/reader.cpp` — based on parser code present in the supplied workspace, adapted for request limits and public-URL checks.
- **pypdfium2 / PDFium** (Apache-2.0 / BSD-3) and **Pillow** (HPND) — only in `scripts/prepare_data.py` to extract text from the provided PDFs.
- **Windows.Media.Ocr** (Windows system API) and optional **Tesseract** (Apache-2.0) — local OCR of PDF pages without a text layer; no external AI services.
- **Debian 12** base image and packages (various free licenses) — Docker build and runtime.
- **Playwright** — used only during development to check the UI in a browser; not part of the delivered solution.
- `src/json.hpp` and `tests/test_harness.hpp` — written for this project.

## Language models (case allowlist only)
Called through the organizers' OpenAI-compatible gateway (`https://changellenge.up.railway.app/v1` or `https://api.alfahack-gateway26.ru/v1`):
- `google/gemma-4-26b-a4b-it` — planning and follow-up questions;
- `deepseek/deepseek-v4.1-flash` — main analysis and revision;
- `z-ai/glm-5.3-flash` — review (self-check).
The other allowed models (`qwen/qwen3.6-35b-a3b`, `qwen/qwen3-vl-30b-a3b-instruct`, `moonshotai/kimi-k2.6`) are accepted by configuration but not used by default. No other LLMs, external AI services or ready-made agent frameworks are used; the agent loop is implemented in this repository.

## Datasets
- Anonymized case data from Alfa-Bank / Changellenge: `df_Uid_data_hack.csv`, `df_Fid_Uid_link_hack.csv`, `df_Uid_payment_hack.csv`.
- Alfa-Bank Center for Macroeconomic Research materials supplied with the case (9 PDF documents), stored as extracted page text in `data/analytics/`.
- `data/news/seed.json` — 31 news items published by Russian media in 2026; titles and short summaries are written by the team, each item links to the original publication. Copyright in the original articles remains with their publishers.

## External services
- RSS feeds of РБК, Интерфакс, ТАСС, Коммерсантъ, Ведомости and the Bank of Russia — read-only, public, at most every 30 minutes.

## Simulated integrations
- **Alfa-Business internet bank** — the main screen, accounts, payments and credits tiles in `ui/` are mock-ups; there is no real authentication or banking data.
- **User–company binding** — the user picks a client from the dataset instead of SSO.
- **Bank product offers** — product names are generic descriptions, not real tariffs.
