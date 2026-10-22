# API для фронтенда

База: тот же адрес, что и страница (`/api/v1`). При локальной разработке фронта на другом порту запустите бэкенд с `ALFA_CORS_ORIGIN=http://localhost:5173`. Собранный фронт кладётся в `ui/` (или указывается через `ALFA_STATIC_DIR`) и раздаётся бэкендом на `/`; неизвестные пути без расширения отдают `index.html` (SPA).

Ключ модели во фронт не передаётся никогда. Если задан `ALFA_API_TOKEN`, токен добавляет серверный прокси.

Примеры реальных ответов: `docs/examples/v2-*.json`. Работающий эталон интерфейса: `ui/index.html`.

## Экраны и вызовы

| Экран | Вызов | Что показать |
|---|---|---|
| Главный экран | `GET /news?limit=1` → `total` | Плитка и пункт меню «Новости для бизнеса» (критерий 09) |
| Выбор роли | `GET /roles` → `items[{id,title,focus,priority_topics}]` | Селектор роли; по умолчанию `cfo` |
| Выбор клиента | `GET /clients?q=&industry=&region=&limit=20&offset=0` → `{total, items[{id, industry, region, city, revenue_rub, products[], foreign_trade, group_size}]}`; фильтры — `GET /clients/facets` | Список с поиском и фильтром отрасли |
| Лента | `GET /companies/{id}/feed?role=cfo&limit=20` → `{role, counterparty_industries[], data_gaps[], items[]}` | Карточки новостей |
| Разбор | `POST /insights {company_id, news_id, role}` (5–25 с с моделью) | Блоки разбора, источники, ход агента |
| Уточнение | `POST /insights/{id}/questions {question?, lens?}` → `{answer, evidence_ids[], method}` | Кнопки разрезов и поле вопроса |
| Оценка | `POST /feedback {insight_id, useful, reason?}` | «Полезно / нет» |

## Карточка ленты (`items[]`)

`news_id`, `title`, `summary`, `url`, `source`, `published` (ISO или `null`), `categories[]` и `category_labels[]`, `duplicates[{url,source}]` (та же история в других изданиях), `why_for_you` (строка «почему вам»), `relevance{score 0..1, band high|medium|low, reasons[]}`, `bank_hint{title, document, page, label}` или `null`.

## Разбор (`POST /insights`)

| Поле | Как показать |
|---|---|
| `facts.what_happened` + `facts.evidence_ids` | Блок «Что произошло» (факт) |
| `interpretation.why_it_matters` + `why_it_matters_evidence_ids` | «Почему это важно вам» |
| `relevance.reasons[]` | Признаки компании, повлиявшие на вывод |
| `interpretation.potential_effects[]`, `risks[]`, `opportunities[]`, `possible_actions[]` | Списки с меткой «Вывод AI»; у элемента `text`, `evidence_ids[]`, `supported_by_cited_source`, иногда `horizon`, `confidence` |
| `interpretation.bank_position` | Отдельный блок «Позиция Альфа-Банка» (`null` → «явной позиции нет») |
| `offers[]` | Предложение банка с пометкой «реклама» (`is_advertising=true`) |
| `interpretation.uncertainties[]` | Свёрнутый список |
| `sources[]` | `id` (N1, C1, P1, R1…, A1…), `type`, `title` или `document`+`page`, `url` |
| `agent_trace[]` | «Как агент пришёл к выводу»: `step`, `status`, `decision`, `duration_ms` |
| `quality_check` | `claims_supported` из `claims_total`, `revise_rounds` |
| `generation` | `method` (`allowlisted_llm` / `deterministic_fallback`), `models{plan,draft,review}` |
| `cached` | Повторное открытие из кэша |

Ссылки `evidence_ids` делайте кликабельными: по `id` найдите источник в `sources[]` и откройте его `url` (новость) или покажите документ и страницу (материал банка).

`lens`: `risks` («Подробнее о рисках»), `counterparties` («Влияние на контрагентов»), `treasury` («Взгляд казначея»), `scenarios` («Сценарии»), `sources` («Первоисточники»).

## Ошибки

`{error, message}` со статусом 400 (неверный ввод, неизвестная роль или разрез), 404 (компания, новость или разбор не найдены), 413 (слишком большой текст), 422 (страница по URL не прочиталась), 503 (перегрузка).

Безопасность: весь текст из API выводите как текст (`textContent`, не `innerHTML`), ссылки — только http/https с `rel="noopener noreferrer"`.
