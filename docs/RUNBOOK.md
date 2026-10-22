# Запуск и обслуживание сервера

Команды выполняются из корня проекта. Собранный Windows backend находится в `x64/Release/AlphaBackend.exe`.

## Быстрый запуск

В PowerShell:

```powershell
Set-Location '<папка-проекта>'
.\start-server.cmd -NoLlm
```

Сервер работает в текущем окне, адрес по умолчанию — `http://127.0.0.1:8080`. Первичная загрузка CSV занимает несколько секунд. Остановка — `Ctrl+C`. Параметр `-NoLlm` включает локальный резервный разбор и исключает вызовы модели независимо от сохранённых ключей.

Проверка из второго окна:

```powershell
Invoke-RestMethod 'http://127.0.0.1:8080/health'
```

Ожидаемый ответ:

```json
{"service":"alpha-news-backend","status":"ok"}
```

`/health` доступен без токена. Это проверка работающего сервера; состав аналитики проверяйте через поиск и `data/analytics/manifest.json`.

## Запуск в фоне и остановка

```powershell
.\start-server.cmd -NoLlm -Background
.\stop-server.cmd
```

Для второго экземпляра используйте другой порт и укажите тот же порт при остановке:

```powershell
.\start-server.cmd -NoLlm -Background -Port 8081
.\stop-server.cmd -Port 8081
```

У команды остановки порт по умолчанию — `8080`; она не читает `.env`. Если изменили порт через `.env` или параметры запуска, передайте этот порт в `stop-server.cmd -Port ...`. Готовая команда остановки выводится при старте.

Фоновый запуск ожидает готовности `/health`, затем выводит адрес и PID. Логи находятся в `work/server/server-8080.stdout.log` и `work/server/server-8080.stderr.log`; файл состояния — `work/server/server-8080.json`. Повторный запуск на занятом порту завершится с понятной ошибкой. Остановка проверяет путь исполняемого файла и время создания процесса; неизвестный процесс на этом порту автоматически не завершается.

Эквивалент без `.cmd`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\start-server.ps1 -NoLlm -Background
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\stop-server.ps1
```

`ExecutionPolicy Bypass` применяется только к этому процессу PowerShell. Скрипты рассчитаны на Windows PowerShell 5.1 и PowerShell 7. В Linux используйте Docker или собранный через CMake backend.

## Конфигурация через .env

Создайте локальный файл, если его ещё нет:

```powershell
if (!(Test-Path -LiteralPath .env)) { Copy-Item -LiteralPath .env.example -Destination .env }
```

Внесите ключи своим локальным редактором. `.env` исключён из Git. Скрипт читает только перечисленные `ALFA_*` настройки, не выполняет содержимое файла и не выводит значения ключей. Поддерживаются строки `NAME=value`, необязательный префикс `export`, одинарные/двойные кавычки вокруг всего значения и комментарии на отдельных строках. Подстановка переменных и escape-последовательности не выполняются; комментарии после значения не поддерживаются. Повторное имя получает последнее значение. Неизвестное имя `ALFA_*` считается ошибкой.

Приоритет: аргументы запуска `-Port` / `-BindAddress`, затем локальный `.env`, затем унаследованные `ALFA_*`, затем значения по умолчанию. Относительные каталоги считаются от корня проекта. Чтобы явно выбрать другой файл:

```powershell
.\start-server.cmd -EnvironmentFile .env.example -NoLlm -Background
```

Если указанный явно файл не существует, запуск завершается с ошибкой. Отсутствующий стандартный `.env` допустим. Файл сам по себе не экспортирует переменные в другие терминалы или frontend: у каждого процесса своё окружение. Изменения настроек вступают в силу после перезапуска backend.

| Переменная | Значение по умолчанию | Назначение |
| --- | --- | --- |
| `ALFA_HOST` | `127.0.0.1` | IPv4-адрес привязки |
| `ALFA_PORT` | `8080` | Порт, 1–65535 |
| `ALFA_DATA_DIR` | `data/input` | Три CSV набора |
| `ALFA_ANALYTICS_DIR` | `data/analytics` | Подготовленные `.md` / `.txt` |
| `ALFA_API_TOKEN` | пусто | Входящий HTTP API: Bearer или `x-api-key` |
| `ALFA_LLM_API_KEY` | пусто | Отдельный ключ шлюза модели |
| `ALFA_LLM_BASE_URL` | `https://changellenge.up.railway.app/v1` | Разрешённый шлюз кейса |
| `ALFA_LLM_MODEL` | `deepseek/deepseek-v4.1-flash` | Модель из разрешённого списка |
| `ALFA_LLM_TIMEOUT_SECONDS` | `35` | Тайм-аут одного вызова, в коде ограничивается 5–90 с |
| `ALFA_AGENT_SELF_CHECK` | `1` | Второй вызов самопроверки; `0`, `false`, `no`, `off` отключают |

Запуск с настроенной моделью:

```powershell
.\start-server.cmd -Background
```

Startup не вызывает модель: расходы возникают при анализе новости и уточняющих вопросах. Уберите `-NoLlm`, только когда команда ИИ подготовила `ALFA_LLM_API_KEY`. Входящий `ALFA_API_TOKEN` и ключ модели — разные значения. Launcher очищает унаследованный `OPENAI_API_KEY` только у дочернего процесса; используется специальный `ALFA_LLM_API_KEY`. При завершении скрипта окружение вызывающего терминала восстанавливается.

## Параметры Windows launcher

| Параметр | Пример | Значение |
| --- | --- | --- |
| `-NoLlm` | `-NoLlm` | Принудительно исключить вызовы модели |
| `-Background` | `-Background` | Запуск без отдельного видимого окна, с логами и состоянием |
| `-Port` | `-Port 8081` | Перекрыть порт окружения |
| `-BindAddress` | `-BindAddress 0.0.0.0` | Перекрыть адрес привязки |
| `-EnvironmentFile` | `-EnvironmentFile .env.example` | Выбрать файл конфигурации |
| `-Configuration` | `-Configuration Debug` | Найти Debug вместо Release |
| `-ReadyTimeoutSeconds` | `-ReadyTimeoutSeconds 120` | Ожидание фонового старта, по умолчанию 60 с |
| `-ProjectRoot` | `-ProjectRoot '<папка-проекта>'` | Корень проекта, по умолчанию определяется по скрипту |

Launcher ищет binary в `x64/<Configuration>/AlphaBackend.exe`, затем в `build/<Configuration>/alpha_backend.exe` и `build/alpha_backend.exe`. Требует все три CSV, чтобы вспомогательные данные не пропали незаметно. Сам native backend умеет запуститься и без файлов платежей/связей, поэтому прямой запуск имеет более слабые проверки.

Для доступа с другого компьютера задайте `-BindAddress 0.0.0.0`, отдельный `ALFA_API_TOKEN` и используйте IP машины с backend. Скрипт не меняет firewall. `127.0.0.1` в браузере коллеги означает компьютер коллеги; для frontend предпочтителен proxy, описанный в [FRONTEND.md](FRONTEND.md).

## Быстрая интеграционная демонстрация

Запустите экземпляр без модели и без входящего токена из `.env.example`:

```powershell
.\start-server.cmd -EnvironmentFile .env.example -NoLlm -Background -Port 8081
```

В другом терминале:

```powershell
$base = 'http://127.0.0.1:8081'
Invoke-RestMethod "$base/api/v1/companies/U_9250"
$query = [Uri]::EscapeDataString('инфляция')
Invoke-RestMethod "$base/api/v1/analytics/search?q=$query&limit=3"
$body = Get-Content -LiteralPath .\docs\examples\demo-news.json -Raw -Encoding UTF8
$insight = Invoke-RestMethod "$base/api/v1/insights" -Method Post -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes($body))
$insight.generation
$insight.sources | Select-Object id,type,document,page
```

В `generation.method` ожидается `deterministic_fallback`; это нормальный результат режима без модели. `C1` — контекст компании, `N1` — введённая новость, `A1..An` — найденные фрагменты аналитики. Пример является синтетическим сценарием, а не реальной публикацией.

После демонстрации:

```powershell
.\stop-server.cmd -Port 8081
```

## Docker

Нужны подготовленные `data/input/*.csv` и `data/analytics/*.md`. Настройки берутся из локального `.env`; Docker Compose передаёт только явно описанные в `docker-compose.yml` значения. Внутри контейнера backend слушает `0.0.0.0:8080`, на машине порт опубликован только на `127.0.0.1:${ALFA_PORT:-8080}`. Каталог `data` подключён только для чтения.

У Compose собственный порядок настройки: значения окружения терминала могут перекрыть `.env`. Адрес и каталоги внутри контейнера заданы его конфигурацией, а `ALFA_PORT` выбирает внешний порт машины.

```powershell
docker compose up --build -d
docker compose logs -f alpha-backend
docker compose down
```

После замены данных: `docker compose restart alpha-backend`. После изменения `.env`: `docker compose up -d --force-recreate`. Не запускайте одновременно Windows-экземпляр и Docker на одном порту.

## Сборка и проверки

Откройте `AlphaBackend.sln`, выберите `Release | x64`, соберите решение. Оно включает `AlphaBackend` и `AlphaPdfOcr`. Нужны C++ workload, Windows SDK и curl через vcpkg; подробности — [README](../README.md).

Тесты индекса:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\analytics_index_tests.exe .\data\analytics
```

Проверки parser-а окружения:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\launcher_environment_tests.ps1
```

Полный локальный цикл запуска, авторизации, поиска, анализа без модели и остановки:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\server_lifecycle_tests.ps1 -Port 18082 -WriteExamples
```

Проверка использует отдельный временный файл с тестовым токеном, принудительно отключает LLM и останавливает свой процесс. `-WriteExamples` обновляет примеры ответов в `docs/examples/`; их `insight_id` относятся к тестовому процессу и не действуют после его остановки.

## Диагностика

| Симптом | Что проверить |
| --- | --- |
| Порт занят | Закройте собственный предыдущий экземпляр или выберите `-Port`; launcher не завершает неизвестные процессы |
| Нет executable / DLL | Соберите `Release | x64`; рядом с binary должны быть `libcurl.dll` и `z.dll` |
| Нет CSV | Запустите `python scripts/prepare_data.py . --data-only`; каталог можно переопределить `ALFA_DATA_DIR` |
| Нет банковских фрагментов | Проверьте `manifest.json`, `.md`, запрос и перезапуск; нет горячей перезагрузки индекса |
| HTTP 401 | Входящий API защищён `ALFA_API_TOKEN`; настройте тот же токен в серверном proxy/клиенте |
| Браузер сообщает CORS | Настройте proxy `/api` и `/health`, не отправляйте запрос на другой origin напрямую |
| HTTP 200, но LLM не использовалась | Проверьте `generation.method`, `generation.warning`, `ALFA_LLM_API_KEY` и модель; наличие ключа не доказывает успешный вызов |
| `insight_not_found` | Анализ исчез после перезапуска или вытеснения; создайте его заново |
| PID в state принадлежит другому процессу | Не останавливайте его по старому PID; проверьте путь и состояние; stale state можно удалить вручную после проверки |

Перезапуск теряет сохранённые в памяти анализы и контекст для уточнений. Остановка фонового процесса принудительная; незавершённые запросы прерываются. Логи API содержат метод, статус и время обработки; тела запросов и ключи в них не записываются.
