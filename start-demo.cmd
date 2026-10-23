@echo off
chcp 65001 >nul
title Novosti dlya biznesa
cd /d "%~dp0"
echo.
echo === Новости для бизнеса: запуск через Docker ===
echo.
docker info >nul 2>&1
if errorlevel 1 (
  echo Docker не отвечает. Откройте Docker Desktop, дождитесь зелёного статуса "Engine running" и запустите этот файл снова.
  pause
  exit /b 1
)
if not exist .env (
  copy /y .env.example .env >nul
  echo Создан файл .env. Чтобы включить AI-модель, впишите ключ организаторов в строку ALFA_LLM_API_KEY= и запустите этот файл снова.
)
echo Сборка и запуск. Первый раз это занимает 3-8 минут: скачивается образ, собирается C++ и прогоняются тесты.
docker compose up --build -d
if errorlevel 1 (
  echo.
  echo Сборка не удалась. Пришлите Claude текст ошибки выше.
  pause
  exit /b 1
)
echo.
echo Ждём, пока сервер загрузит данные...
set /a tries=0
:wait
set /a tries+=1
if %tries% gtr 60 (
  echo Сервер не ответил за 3 минуты. Логи: docker compose logs
  pause
  exit /b 1
)
timeout /t 3 /nobreak >nul
curl -fsS http://127.0.0.1:8080/health >nul 2>&1 || goto wait
echo.
echo Готово: http://127.0.0.1:8080
start "" http://127.0.0.1:8080
echo Остановить сервис: stop-demo.cmd
echo.
pause
