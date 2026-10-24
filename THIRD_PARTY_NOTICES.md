# Third-party components, models, datasets, services and simulated integrations

## Software
- **libcurl** (curl license, MIT-like) — HTTP(S) for news pages, RSS feeds and the model gateway. Linked dynamically; not vendored. Build-time headers come from the system package (`libcurl4-openssl-dev`).
- **HTML parser and readability/Markdown conversion** in `src/reader.cpp` — based on parser code present in the supplied workspace, adapted for request limits and public-URL checks.
- **pypdfium2 / PDFium** (Apache-2.0 / BSD-3) and **Pillow** (HPND) — only in `scripts/prepare_data.py` to extract text from the provided PDFs.
- **Windows.Media.Ocr** (Windows system API) and optional **Tesseract** (Apache-2.0) — local OCR of PDF pages without a text layer; no external AI services.
- **Debian 12** base image and packages (various free licenses) — Docker build and runtime.
- **Playwright** (Apache-2.0) — only for the optional browser test `tests/ui/ui_e2e.py`; not part of the delivered service.
- **Node.js** test runner — only for `tests/ui/core.test.cjs`; not part of the delivered service.
- **Golos Text** font (SIL Open Font License 1.1, The Golos Text Project Authors) — bundled in `ui/assets/GolosText.ttf`; license in `ui/assets/OFL.txt`.
- `src/json.hpp`, `tests/test_harness.hpp` and the UI in `ui/` (plain JavaScript, no frameworks) — written for this project.

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
- Organizers' model gateway (Changellenge, `changellenge.up.railway.app` / `api.alfahack-gateway26.ru`) — the only AI service; the key stays on the backend.
- **SnapDeploy** (AAR Labs) — free hosting of the public demo built from this repository's `Dockerfile`; no code or data beyond the repository is uploaded, the model key is a container secret.
- **Cloudflare Quick Tunnel** via the `cloudflare/cloudflared` image (Apache-2.0) — optional public link from a developer machine (`start-public.cmd`).
- RSS feeds of РБК, Интерфакс, ТАСС, Коммерсантъ, Ведомости and the Bank of Russia — read-only, public, at most every 30 minutes.

## Simulated integrations
- **Alfa-Business internet bank** — the home screen with accounts, payments and credits tiles (marked «демо») is a mock-up; there is no real authentication or banking data.
- **Scenario calculator** — computes only from user-entered amount and rates; no market data feed.
- **User–company binding** — the user picks a client from the dataset instead of SSO.
- **Bank product offers** — product names are generic descriptions, not real tariffs.
