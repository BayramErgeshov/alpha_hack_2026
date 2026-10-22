# Персональная лента и агентный разбор — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (выбран Native: капитан делегировал выполнение, срок — 1,5 дня). Шаги отмечаются `- [ ]`.

**Goal:** Добавить к C++ бэкенду список клиентов, сбор новостей, персональную ленту, агентный цикл с самопроверкой, разрезы, предложения банка, обратную связь и метрики.

**Architecture:** Один процесс C++20. Новые модули `news_store`, `rss`, `feed`, `budget`; агент `InsightAgent` переписывается на цикл plan → tools → draft → verify → revise. Состояние в `work/*.json`.

**Tech Stack:** C++20, libcurl, CMake, CTest; Python только для тестового LLM-сервера.

**Spec:** `docs/superpowers/specs/2026-10-23-news-insights-design.md`

## Global Constraints

- Только модели `google/gemma-4-26b-a4b-it`, `z-ai/glm-5.3-flash`, `qwen/qwen3.6-35b-a3b`, `deepseek/deepseek-v4.1-flash`, `qwen/qwen3-vl-30b-a3b-instruct`, `moonshotai/kimi-k2.6`; шлюзы `https://changellenge.up.railway.app/v1`, `https://api.alfahack-gateway26.ru/v1`.
- Порог крупного бизнеса: выручка ≥ 15 000 000 000 руб.
- Без новых сторонних библиотек C++.
- Ключ модели не попадает в ответы API, логи, фронт и репозиторий.
- Тестовый HTTP-шлюз модели разрешён только при `ALFA_LLM_TEST_ENDPOINT=1` и только на `http://127.0.0.1:<port>`.

## Review Focus

- Новость без даты или с датой в неизвестном формате → считается свежей наполовину, не падает и не уходит в конец ленты. Тест: `feed_handles_missing_dates`.
- Компания без отрасли и региона → лента и разбор работают, причины отмечают неизвестные поля. Тест: `feed_company_with_blank_fields`.
- Модель вернула не-JSON или JSON без нужных полей на любом шаге → детерминированный запасной вариант на этом шаге, trace показывает `fallback`. Тест: e2e сценарий `broken_model_reply`.
- RSS с CDATA, HTML-сущностями и Atom вместо RSS 2.0 → корректные заголовки без тегов. Тест: `rss_parses_cdata_entities_and_atom`.
- Исчерпан дневной лимит токенов → разбор не вызывает модель, возвращает `budget_exhausted` в trace. Тест: `budget_blocks_after_limit`.

---

### Task 1: Список клиентов

**Files:** Modify `src/data_store.hpp`, `src/data_store.cpp`, `src/http_server.cpp`; Test `tests/backend_tests.cpp` (новый, CTest `backend`).

**Interfaces — Produces:**
- `struct ClientFilter { std::string query, industry, region; double min_revenue = 15e9; size_t limit = 50, offset = 0; };`
- `json::Value DataStore::list_clients(const ClientFilter&) const` → `{total, limit, offset, items:[{id, industry, okved_code, region, city, revenue_rub, client_type, products:[string], group_size}]}`, сортировка по выручке убыв.
- `json::Value DataStore::client_facets(double min_revenue) const` → `{industries:[{name,count}], regions:[{name,count}]}`, по 30 самых частых.
- `std::vector<std::string> DataStore::counterparty_industries(const CompanyProfile&, size_t top) const`.
- `GET /api/v1/clients`, `GET /api/v1/clients/facets`.

- [ ] Тест `clients_filtered_by_threshold_and_sorted`: все `revenue_rub ≥ 15e9`, убывание, `total ≥ items.size()`; фильтр по отрасли сужает выборку.
- [ ] Реализация, сборка, тест зелёный, коммит.

### Task 2: Хранилище новостей и RSS

**Files:** Create `src/news.hpp`, `src/news.cpp`, `data/news/seed.json`; Modify `src/reader.hpp/.cpp` (`bool fetch_public_text(const std::string& url, long timeout, std::string& body, std::string& error)` с защитой SSRF и редиректами); Test `tests/backend_tests.cpp`.

**Interfaces — Produces:**
- `struct NewsItem { std::string id, title, summary, url, source, published; long long published_epoch = 0; std::vector<std::string> categories; std::vector<json::Value> duplicates; };`
- `std::vector<NewsItem> parse_feed_xml(const std::string& xml, const std::string& source)` (RSS 2.0 и Atom).
- `class NewsStore { bool load_seed(path, err); bool add(NewsItem) /*false = дубль, слит*/; std::vector<NewsItem> recent(size_t) const; const NewsItem* find(id) const; size_t size() const; void save(path) const; void load_state(path); }` — потокобезопасный.
- `std::vector<std::string> classify_news(const std::string& text)` — ключи категорий `rate,currency,commodity,logistics,sanctions,demand,regulation,labor,technology,macro`.
- `class NewsCollector { start(NewsStore&, minutes); json::Value refresh_now(NewsStore&); }` — источники из `default_news_sources()`.

- [ ] Тесты `rss_parses_cdata_entities_and_atom`, `news_dedup_merges_same_story`, `classify_rate_news`.
- [ ] Реализация, демо-набор ≥ 30 новостей, тесты, коммит.

### Task 3: Лента

**Files:** Create `src/feed.hpp`, `src/feed.cpp`; Modify `src/agent.hpp/.cpp` (вынести `rank` в свободную функцию `json::Value score_relevance(const CompanyProfile&, const std::string& title, const std::string& text, const std::vector<std::string>& counterparty_industries, const std::string& role)`), `src/http_server.cpp`.

**Interfaces — Produces:**
- `const std::vector<RoleInfo>& roles()`; `RoleInfo { id, title, focus, std::map<std::string,double> weights }`; id: `owner, ceo, cfo, treasurer, accountant`.
- `json::Value build_feed(const DataStore&, const AnalyticsIndex&, const NewsStore&, const CompanyProfile&, const std::string& role, size_t limit)` → `{company_id, role, items:[{news_id,title,source,url,published,summary,categories,relevance,why_for_you,bank_hint,duplicates}]}`.
- `GET /api/v1/roles`, `GET /api/v1/news`, `POST /api/v1/news/refresh`, `GET /api/v1/companies/{id}/feed`.

- [ ] Тесты `feed_changes_with_company`, `feed_changes_with_role`, `feed_handles_missing_dates`, `feed_company_with_blank_fields`.
- [ ] Реализация, тесты, коммит.

### Task 4: Агентный цикл, бюджет, маршрутизация моделей

**Files:** Create `src/budget.hpp/.cpp`; Modify `src/llm_client.hpp/.cpp` (`LlmReply::prompt_tokens, completion_tokens`; `LlmConfig::fast_model, review_model`; `llm_chat_model(config, model, system, user, max_tokens)`; тестовый шлюз), `src/agent.hpp/.cpp`.

**Interfaces — Produces:**
- `class TokenBudget { bool allow(); void record(int prompt, int completion, const std::string& model); json::Value stats() const; }` — дневной лимит `ALFA_LLM_DAILY_TOKEN_LIMIT` (по умолчанию 400000), сброс в 03:00 МСК.
- `json::Value verify_claims(const json::Value& draft, const std::unordered_set<std::string>& ids, const std::string& sources_text)` → `{ok, issues:[{code, text}]}`; коды `no_evidence`, `bank_view_without_bank_source`, `number_not_in_sources`, `empty_section`.
- `InsightAgent::analyze(company_id, ArticleInput, role)`; кэш по `(company, sha(title+text), role)`.
- В ответе: `agent_trace[]` элементы `{step, status, decision, detail, duration_ms}`; `offers[]`; `role`.

- [ ] Тесты: e2e через `tests/mock_llm.py` — `happy_path_uses_three_models`, `reviewer_requests_more_search_triggers_revise`, `broken_model_reply`; unit `budget_blocks_after_limit`, `verifier_flags_claim_without_source`.
- [ ] Реализация, тесты, коммит.

### Task 5: Разрезы, предложения, обратная связь, метрики, статика

**Files:** Modify `src/agent.cpp`, `src/http_server.cpp`; Create `src/metrics.hpp/.cpp`.

**Interfaces — Produces:**
- `follow_up(insight_id, question, lens)`; lens: `risks, counterparties, treasury, scenarios, sources`.
- `json::Value bank_offers(const CompanyProfile&, const std::vector<std::string>& categories)` → `[{product, text, reason, label:"Предложение Альфа-Банка"}]`.
- `class Metrics { record_insight(json); record_feedback(id, useful, reason); json::Value snapshot(const TokenBudget&) const; }`.
- `POST /api/v1/feedback`, `GET /api/v1/metrics`; статика из `ALFA_STATIC_DIR`; CORS из `ALFA_CORS_ORIGIN`.

- [ ] Тесты: `lens_question_without_llm`, `offers_for_trade_company`, `feedback_updates_metrics`, `static_blocks_path_traversal`.
- [ ] Реализация, тесты, коммит.

### Task 6: Упаковка

- [ ] Dockerfile (сборка + рантайм, `0.0.0.0`, данные внутри образа), docker-compose, `.env.example` с новыми переменными, `openapi.yaml`, README для жюри, THIRD_PARTY_NOTICES, `docs/METRICS.md`, `docs/DEMO.md`, `docs/FRONTEND_API.md`, тег `v1.0`.
