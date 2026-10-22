# Подключение фронтенда

Начните с [запуска сервера](RUNBOOK.md) из корня проекта backend. Описание проекта находится в [README](../README.md), перечень маршрутов — в [openapi.yaml](../openapi.yaml). Ниже описан фактический JSON-контракт и особенности интерфейса.

## Быстрый старт

1. Запустите backend без платных вызовов ИИ:

   ```powershell
   Set-Location '<папка-проекта>'
   .\scripts\start-server.ps1 -NoLlm
   ```

2. Скопируйте [пример конфигурации Vite](examples/vite.config.ts) в корень проекта фронтенда как `vite.config.ts`. Это исходник конфигурации: зависимости `vite` и TypeScript должны быть установлены в проекте фронтенда. Если у вас уже настроены React/Vue-плагины, перенесите в существующий конфиг блок `server.proxy` и чтение `ALFA_*`.
3. В отдельном окне PowerShell из папки фронтенда запустите ваш обычный dev-сервер. Пример для проекта со стандартным скриптом Vite:

   ```powershell
   $env:ALFA_BACKEND_URL = 'http://127.0.0.1:8080'
   # Только если backend запущен с ALFA_API_TOKEN:
   # $env:ALFA_API_TOKEN = '<тот же локальный токен доступа к backend>'
   npm run dev
   ```

4. Фронтенд обращается к относительным путям `/health` и `/api/v1/...`. С браузера открывается адрес Vite, например `http://127.0.0.1:5173`. Запрос `fetch('/health')` должен вернуть `{ "status": "ok", "service": "alpha-news-backend" }`.

Вместо переменных PowerShell можно задать `ALFA_BACKEND_URL` и `ALFA_API_TOKEN` в локальном `.env.local` **проекта фронтенда**; пример конфигурации читает их только внутри процесса Vite. Этот файл необходимо исключить из Git. Изменения окружения требуют перезапуска Vite.

Backend не реализует CORS и обработку `OPTIONS`. При прямом запросе браузера с порта фронтенда на порт `8080` возникнет ошибка браузера; используйте proxy Vite. В развёрнутом приложении настройте общий origin и reverse proxy для `/api/` и `/health`. Proxy должен передавать обычное тело JSON с `Content-Length`: `Transfer-Encoding: chunked` сервер не принимает. Тайм-аут upstream задайте не менее 240 секунд.

## Токены и окружение

| Настройка | Где используется | Что передавать в браузер |
| --- | --- | --- |
| `ALFA_API_TOKEN` | Необязательная защита HTTP API backend; серверный proxy добавляет `Authorization: Bearer ...` | Не включать в bundle или `VITE_*` |
| `ALFA_LLM_API_KEY` | Ключ модели, только окружение backend | Никогда |
| `ALFA_BACKEND_URL` | Адрес backend в примере конфигурации Vite; backend эту переменную не читает | Клиенту достаточно относительных `/api/...` |

Это разные секреты. Ключ модели не подходит для авторизации HTTP API. При заданном `ALFA_API_TOKEN` все маршруты `/api/v1/` требуют `Authorization: Bearer <токен>` или `x-api-key: <токен>`. `/health` открыт. При пустом токене API работает без авторизации. В примере Vite токен добавляется proxy на сервере, поэтому клиентский код не содержит заголовка авторизации. Proxy рассчитан на локальную разработку с привязкой к `127.0.0.1`; в production нужна собственная схема доступа пользователей.

## Предлагаемый сценарий интерфейса

1. **Компания.** Пользователь вводит синтетический ID, например `U_9250`, либо точный ОКВЭД. Запрос `/api/v1/companies?q=U_9250` возвращает карточки. Текущая реализация ищет только точное совпадение ID или ОКВЭД: поиска по названию, подстроке, фильтров и пагинации нет. В данных отсутствуют реальные названия компаний, показывайте ID и отрасль.
2. **Контекст компании.** Загрузите профиль; при необходимости отдельно сеть платежей. Отображайте неизвестные поля как «Нет данных». Процентиль выручки в наборе не называйте долей рынка.
3. **Новость.** Два режима ввода: публичный URL либо вставленный текст. Кнопка предварительного просмотра URL может вызвать `/api/v1/parse`; анализ URL также сам выполнит извлечение страницы. Предварительный parse не кэшируется и последующий анализ URL загрузит страницу ещё раз.
4. **Анализ.** Отправьте `company_id` и URL либо текст в `/api/v1/insights`. Покажите загрузку; сохраните полученный объект в состоянии интерфейса. Для каждой карточки вывода отобразите ссылки на `evidence_ids`.
5. **Уточнение.** Отправьте вопрос в `/api/v1/insights/{id}/questions`, используя `id` из ответа. При `insight_not_found` предложите заново создать анализ. Вопросы отвечают по одному сохранённому анализу; история предыдущих уточнений на сервере не накапливается.

Готовой новостной ленты, списка всех компаний, истории анализов, загрузки файлов, потоковой выдачи токенов и маршрута скачивания PDF в API нет. Документ с именем из `sources[].document` нельзя автоматически превратить в рабочий URL backend.

## Маршруты

Ответы — JSON UTF-8. POST отправляйте с `Content-Type: application/json`. Параметры пути и query кодируйте через `encodeURIComponent` / `URLSearchParams`.

| Метод и путь | Вход | Успешный ответ |
| --- | --- | --- |
| `GET /health` | — | `{status:"ok", service:"alpha-news-backend"}` |
| `GET /api/v1/models` | — | `{configured:boolean, models:string[]}` |
| `GET /api/v1/companies?q=...&limit=20` | `q` обязателен; `limit` 1–50, по умолчанию 20 | `{items: CompanyProfile[]}`; отсутствие совпадений — пустой массив |
| `GET /api/v1/companies/{company_id}` | ID, 1–128 ASCII-букв/цифр, `_` и `-` | `CompanyProfile` |
| `GET /api/v1/companies/{company_id}/network` | тот же ID | `CompanyNetwork` |
| `GET /api/v1/analytics/search?q=...&limit=5` | `q` обязателен; `limit` 1–20 | `{items: AnalyticsHit[]}` |
| `POST /api/v1/parse` | `{url:string, mode?:"jina"|"raw"}` | `ParsedPage` |
| `POST /api/v1/insights` | `InsightRequest` | `Insight` |
| `POST /api/v1/insights/{insight_id}/questions` | `{question:string}` | `FollowUp` |

`models.configured` означает наличие ключа в окружении, а не проверенную работоспособность модели или доступный бюджет. Модель выбирается настройкой backend, поле `model` в запросе анализа не поддерживается.

### Профиль и сеть компании

```ts
type Nullable<T> = T | null;

interface CompanyProfile {
  id: string;
  client_type: Nullable<string>;
  organization_type: Nullable<string>;
  ownership: Nullable<string>;
  company_age_band: Nullable<string>;
  development_stage: Nullable<string>;
  industry: {
    okved_code: Nullable<string>;
    name: Nullable<string>;
    category: string;
  };
  financials: {
    annual_revenue_rub: Nullable<number>;
    case_large_business_threshold_rub: number; // 15_000_000_000
    meets_case_large_business_threshold: boolean;
    revenue_percentile_in_dataset_industry: Nullable<number>; // 0–100
    market_share: null;
    market_share_note: string;
  };
  employees: { fns_band: Nullable<string>; spark_band: Nullable<string> };
  location: { city: Nullable<string>; region: Nullable<string> };
  products: {
    rko: boolean; bank_guarantees: boolean; loans: boolean;
    leasing: boolean; deposits: boolean; salary_project: boolean; factoring: boolean;
  };
  trade: { has_fts_data: boolean; fts_country: Nullable<string> };
  network: {
    group_id: Nullable<string>;
    group_member_count: number;
    related_company_count: number;
    linked_person_record_count: number;
  };
  payments: PaymentTotals;
  data_quality: {
    revenue_available: boolean; industry_available: boolean;
    location_available: boolean; employee_data_available: boolean; warning: string;
  };
  source: string;
}

interface PaymentTotals {
  incoming_amount_rub: number;
  outgoing_amount_rub: number;
  incoming_transaction_count: number;
  outgoing_transaction_count: number;
}

interface CompanyNetwork {
  company_id: string;
  group_id: Nullable<string>;
  group_member_count: number;
  related_company_count: number;
  linked_person_record_count: number;
  payment_totals: PaymentTotals;
  top_counterparties: Array<{
    company_id: string;
    direction: 'incoming' | 'outgoing'; // относительно выбранной компании
    amount_rub: number;
    payment_count: number;
    profile_available: boolean;
  }>;
  source: string;
}
```

`top_counterparties` содержит максимум восемь записей по сумме, обе стороны платежей входят в один список. У одного контрагента могут быть две записи с разным направлением. Открывайте профиль контрагента только при `profile_available=true`. Платежные итоги агрегированы, дат операций и назначения платежа нет. Суммы не означают выручку за год. У признаков продуктов тип `boolean`, но `false` не доказывает отсутствие продукта: исходные пустые значения также нормализуются в `false`.

### Парсер и банковская аналитика

```ts
interface ParsedPage {
  url: string;
  title: string; // может быть ""
  published_time: string; // может быть ""; формат источника не гарантирован
  http_status: number;
  markdown: string;
  content_markdown: string;
}

interface AnalyticsHit {
  id: string;
  document: string;
  page: number; // номер страницы исходного PDF, начиная с 1
  score: number; // BM25, не вероятность
  text: string;
  text_extraction: string; // например pdf_text, windows_ocr или tesseract_ocr
}
```

`mode="jina"` — режим по умолчанию, `markdown` содержит заголовки `Title`, `URL Source`, `Published Time` (при наличии) и `Markdown Content`. `content_markdown` содержит только материал. `mode="raw"` возвращает извлечённый Markdown без этой обёртки; это не исходный HTML. HTML с исполнением JavaScript, авторизация на сайтах и обход платного доступа не реализованы. Допускаются публичные HTTP(S)-адреса; локальные и приватные адреса блокируются.

Банковские фрагменты с OCR могут содержать ошибки в цифрах и таблицах. Показывайте имя документа, страницу и способ извлечения; сомнительные числовые значения требуют сверки с исходным PDF.

### Создание анализа

```ts
interface InsightRequest {
  company_id: string;
  url?: string;
  text?: string;
  title?: string;
  published_time?: string;
  source_name?: string;
}

interface Claim {
  text: string;
  evidence_ids: string[];
  supported_by_cited_source: boolean;
  source_type: string;
  horizon?: string;
  confidence?: number;
}

interface EvidenceSource {
  id: string;
  type: 'external_news' | 'user_provided_text' | 'company_profile' | 'alfa_bank_analytics';
  title?: string;
  url?: Nullable<string>;
  published_time?: Nullable<string>;
  company_id?: string;
  document?: string;
  page?: number;
  excerpt: string;
  relevance_score?: number;
  text_extraction?: string;
  verified_by_service?: boolean;
}

interface Insight {
  id: string;
  schema_version: '1.0';
  company_id: string;
  article: {
    url: Nullable<string>; title: Nullable<string>;
    published_time: Nullable<string>; source_name: string;
  };
  relevance: {
    score: number; // 0–1, выше = приоритетнее
    band: 'high' | 'medium' | 'low';
    interpretation: string;
    event_categories: string[];
    reasons: string[];
    industry_match: boolean;
    source: string;
  };
  event_categories: string[];
  company_context: {
    industry: string; okved_code: string; industry_category: string;
    region: Nullable<string>; revenue_rub: Nullable<number>;
    revenue_percentile_in_industry_dataset: Nullable<number>;
    meets_case_threshold: boolean; client_type: string;
    credit_products: boolean; leasing: boolean; bank_guarantees: boolean;
    factoring: boolean; foreign_trade_data: boolean;
    payment_network_available: boolean; group_member_count: number;
    related_company_count: number; missing_data_note: string;
  };
  facts: {
    what_happened: string;
    evidence_ids: string[];
    potential_effects?: Claim[]; // может присутствовать при использовании модели
  };
  bank_analytics: EvidenceSource[];
  interpretation: {
    why_it_matters: string;
    potential_effects: Claim[];
    risks: Claim[];
    opportunities: Claim[];
    possible_actions: Claim[];
    bank_position: Nullable<{ text: string; evidence_ids: string[]; label: string }>;
    model_conclusions: Claim[];
    uncertainties: string[];
  };
  sources: EvidenceSource[];
  agent_trace: Array<{ step: string; status: string; note: string }>;
  quality_check: {
    source_linked: boolean; bank_and_model_views_separated: boolean;
    market_share_inferred: boolean; llm_used: boolean;
    self_check_passed: boolean; company_profile_has_missing_fields: boolean;
    scope_note: string;
  };
  generation: {
    method: 'allowlisted_llm' | 'deterministic_fallback';
    model: Nullable<string>;
    self_check: 'passed' | 'not_completed' | 'skipped';
    warning?: string;
  };
}

interface FollowUp {
  insight_id: string;
  question: string;
  answer: string;
  evidence_ids: string[];
  method: 'allowlisted_llm' | 'deterministic_fallback';
}
```

Запрос по тексту:

```json
{
  "company_id": "U_9250",
  "title": "Изменение ключевой ставки",
  "text": "Вставьте полный текст новости, который нужно разобрать.",
  "source_name": "Текст, предоставленный пользователем"
}
```

Запрос по URL: `{ "company_id": "U_9250", "url": "https://example.org/news" }`. Этот адрес — образец, его нужно заменить адресом реальной статьи.

Если передать и `url`, и `text`, backend всегда загрузит URL. Переданный `text` при этом останется основным текстом, а пустые `title`, `published_time`, `source_name` будут дополнены результатом загрузки. Не объединяйте URL одной статьи и текст другой. Для режима вставленного текста оставляйте `url` отсутствующим или пустым. Даты — необязательные строки; backend их не нормализует, учитывайте невозможность распарсить дату в интерфейсе.

## Как показывать результат и источники

Готовые ответы локального API: [профиль компании](examples/company-response.json), [поиск аналитики](examples/analytics-search-response.json), [разбор синтетической новости](examples/insight-response.json), [уточнение](examples/followup-response.json). Примеры получены без модели; `insight_id` относятся к уже остановленному тестовому процессу. Для работы с уточнениями создайте собственный анализ и используйте его новый `id`.

| Поле | Название в интерфейсе / правило |
| --- | --- |
| `facts.what_happened` | «Что произошло», рядом `facts.evidence_ids` |
| `interpretation.why_it_matters` | «Почему это важно для компании» |
| `relevance.score`, `band`, `reasons` | «Релевантность»: высокая от 0.65, средняя от 0.40, иначе низкая; пояснения из `reasons` |
| `potential_effects`, `risks`, `opportunities`, `possible_actions` | Отдельные блоки; пустой массив — «Недостаточно данных», без придуманных элементов |
| `bank_position` | «Позиция из материалов Альфа-Банка» только при непустом объекте; при `null` — «Подтверждённая позиция банка не сформирована» |
| `model_conclusions` | «Выводы модели» для `allowlisted_llm`; «Выводы по правилам» для fallback |
| `uncertainties` | «Ограничения и неизвестные данные» |
| `generation.method` | Явная отметка «Модель» или «Разбор по правилам» |
| `agent_trace` | Раскрываемый список выполненных этапов; это не поток прогресса |

Релевантность — оценка приоритета новости, а не вероятность убытка. Не подписывайте `score * 100` как «вероятность риска». BM25 `sources[].relevance_score` и `AnalyticsHit.score` — другая шкала, не смешивайте её с персональной `relevance.score`.

Ссылки `N1`, `C1`, `A1` и далее действуют **внутри одного анализа**:

- `N1` — новость или вставленный пользователем текст.
- `C1` — обезличенный контекст выбранной компании; переход к карточке `company_context` / профилю компании.
- `A1..An` — фрагменты аналитики Альфа-Банка, найденные для этого анализа.

`sources` — общий список для разрешения ссылок, `bank_analytics` — банковское подмножество. Идентификаторы результатов отдельного `/analytics/search` не подставляйте вместо `A1..An`. Показ найденного банковского фрагмента сам по себе не означает, что банк подтверждает все выводы анализа. `bank_position` — выделенный backend вывод по найденным материалам; он не является новой официальной публикацией банка.

`supported_by_cited_source` проверяет наличие ссылок, а не истинность утверждения. `N1.verified_by_service=true` означает загрузку URL сервисом; это не независимая проверка фактов новости. `quality_check` и `self_check_passed` — технические признаки обработки, их не следует подавать как гарантию достоверности.

```ts
function sourceLabel(source: EvidenceSource): string {
  if (source.type === 'company_profile') return 'Профиль компании';
  if (source.type === 'alfa_bank_analytics') {
    return `${source.document ?? 'Аналитика Альфа-Банка'}, стр. ${source.page ?? '—'}`;
  }
  return source.type === 'user_provided_text' ? 'Текст пользователя' : 'Внешняя новость';
}

function resolveEvidence(insight: Insight, id: string): EvidenceSource | undefined {
  return insight.sources.find(source => source.id === id);
}
```

Все тексты источников недоверенные. Отображайте строки как текст. При рендеринге Markdown используйте очистку HTML, отключите исполнение встроенного HTML и разрешайте для ссылок только `http:` / `https:`. Внешним ссылкам задавайте `rel="noopener noreferrer"` при `target="_blank"`.

## Пример клиента

Пример использует относительные URL и серверный proxy. Он не содержит секретов. Типы выше можно вынести в `api-types.ts` своего проекта; они описывают контракт и сами по себе не выполняют проверку JSON во время работы.

```ts
type ApiFailure = { error?: string; message?: string };

export class ApiError extends Error {
  constructor(readonly status: number, readonly code: string, message: string) {
    super(message);
  }
}

async function api<T>(path: string, init: RequestInit = {}, timeoutMs = 240_000): Promise<T> {
  const controller = new AbortController();
  const timer = window.setTimeout(() => controller.abort(), timeoutMs);
  try {
    const headers = new Headers(init.headers);
    if (init.body && !headers.has('Content-Type')) headers.set('Content-Type', 'application/json');
    const response = await fetch(path, {
      ...init,
      headers,
      signal: controller.signal,
    });
    // В том числе обрабатываем HTML-ошибку reverse proxy, например 502/504.
    const raw = await response.text();
    let payload: unknown;
    try { payload = raw ? JSON.parse(raw) : {}; }
    catch { throw new ApiError(response.status, 'invalid_response', 'Сервер вернул ответ не в формате JSON'); }
    const failure: ApiFailure = payload && typeof payload === 'object' ? payload as ApiFailure : {};
    if (!response.ok || failure.error) {
      throw new ApiError(response.status, failure.error ?? 'http_error', failure.message ?? `HTTP ${response.status}`);
    }
    return payload as T;
  } finally {
    window.clearTimeout(timer);
  }
}

export const searchCompanies = (query: string) => {
  const params = new URLSearchParams({ q: query.trim(), limit: '20' });
  return api<{ items: CompanyProfile[] }>(`/api/v1/companies?${params}`, {}, 15_000);
};

export const getCompany = (id: string) =>
  api<CompanyProfile>(`/api/v1/companies/${encodeURIComponent(id)}`, {}, 15_000);

export const analyzeNews = (input: InsightRequest) =>
  api<Insight>('/api/v1/insights', { method: 'POST', body: JSON.stringify(input) });

export const askQuestion = (insightId: string, question: string) =>
  api<FollowUp>(`/api/v1/insights/${encodeURIComponent(insightId)}/questions`, {
    method: 'POST', body: JSON.stringify({ question: question.trim() }),
  });

// Использование после выбора компании и заполнения формы:
const result = await analyzeNews({ company_id: 'U_9250', text: newsText, title: newsTitle });
if (result.generation.method === 'deterministic_fallback') {
  // Показать полноценный полученный результат и отметку «Разбор по правилам».
}
// Вопрос возможен с result.id до перезапуска/вытеснения записи на backend.
const answer = await askQuestion(result.id, 'Какие данные нужны для оценки долговой нагрузки?');
```

При анализе URL загрузка страницы может занять 25 секунд на запрос, а перенаправления добавляют загрузки. Генерация и самопроверка — до двух последовательных обращений к модели; тайм-аут одного по умолчанию 35 секунд, допустимая настройка backend до 90 секунд. 240 секунд — стартовая настройка клиента/proxy; при длинных цепочках перенаправлений её может потребоваться увеличить. На время POST блокируйте повторное нажатие кнопки. Не повторяйте анализ автоматически после клиентского тайм-аута: backend мог продолжить работу, и повтор потратит бюджет модели ещё раз. Отмена `fetch` не гарантирует отмену обработки на backend.

Без ключа, при недоступной модели или неразбираемом ответе модели анализ возвращается с **HTTP 200**, `generation.method="deterministic_fallback"`, `quality_check.llm_used=false` и возможным `generation.warning`. Это корректный режим интерфейса. Аналогично уточняющий вопрос может вернуть `method="deterministic_fallback"` с пояснением. Не показывайте такие ответы как сетевую ошибку.

## Ограничения и ошибки

Все размеры строк ниже проверяются по числу **байт UTF-8**, хотя часть сообщений об ошибках и OpenAPI используют слово «символы». Для кириллицы лимит видимых символов обычно примерно вдвое меньше.

| Вход | Ограничение backend |
| --- | --- |
| HTTP-заголовки | 65 536 байт |
| Всё тело HTTP-запроса | 2 097 152 байта |
| `insights.text` | 30 000 байт; после приёма агент сокращает текст примерно до 14 000 байт, вход модели — примерно до 10 000 |
| `insights.title` | 1 000 байт; агент сокращает примерно до 400 |
| `insights.url` | 4 096 байт |
| `questions.question` | 2 000 байт, требуется непустой текст |
| Одновременные соединения | 64, далее `503 busy` |

Проверяйте лимиты на клиенте через `new TextEncoder().encode(value).length`. Ограничение поля HTML `maxLength` считает не UTF-8-байты; одной этой проверки недостаточно. Для входа по URL ограничение текста применяется уже к извлечённой статье, поэтому очень длинная статья может дать `413`.

Формат ошибки: `{ "error": "код", "message": "пояснение" }`. Для отображения лучше сопоставлять коды с русскими сообщениями, техническое `message` оставлять в раскрываемых деталях.

| HTTP | Коды / ситуация | Действие интерфейса |
| --- | --- | --- |
| `400` | `query_required`, `invalid_company_id`, `invalid_insight_id`, `invalid_json`, `url_required`, `article_required`, `article_empty`, `question_empty`, `bad_request` | Исправить поле/формат; для пустого вопроса не выполнять запрос |
| `401` | `unauthorized` | Проверить серверный `ALFA_API_TOKEN` proxy и backend, перезапустить proxy после изменения |
| `404` | `company_not_found` | Предложить другой ID компании |
| `404` | `insight_not_found` | Создать анализ заново: память сервера больше не содержит этот ID |
| `404` | `not_found` | Проверить маршрут и метод; неверный метод тоже даёт 404 |
| `413` | `article_too_large`, `question_too_large`, либо `bad_request` для слишком большого HTTP-тела | Сократить входные данные |
| `422` | `parse_failed` | Предложить другой публичный URL или режим вставленного текста |
| `431` | `bad_request` | Сократить заголовки; не проксировать лишние cookies |
| `500` | `internal_error` | Показать ошибку сервера; при повторе POST учитывать возможное повторное списание бюджета |
| `503` | `busy` | Сервер занят; предложить повторить позже |

Инсайты хранятся только в памяти процесса, максимум 500 записей. При заполнении запись для удаления выбирается внутренним контейнером без гарантии порядка; это не TTL и не гарантированная очередь последних 500 анализов. Перезапуск удаляет все анализы. ID нельзя считать постоянной ссылкой на историю. Сохранённую на клиенте карточку можно продолжать показывать, но для уточнения после потери серверного контекста требуется новый анализ.

## Что передать другим участникам

Для обмена с командой данных используйте `company_id`, `company_context` и исходные ID/номера страниц из `sources`, не реальные названия или персональные идентификаторы. Для команды ИИ сохраняйте полный JSON конкретного анализа с `generation`, `quality_check` и `agent_trace`, а также исходный запрос без секретов. Скриншот одного вывода не показывает, какой режим генерации и какие источники были использованы.
