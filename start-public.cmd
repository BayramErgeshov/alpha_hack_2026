@echo off
chcp 65001 >nul
title Novosti dlya biznesa - public link
cd /d "%~dp0"
echo.
echo === Новости для бизнеса: публичная ссылка через Cloudflare ===
echo.
docker info >nul 2>&1
if errorlevel 1 (
  echo Docker не отвечает. Откройте Docker Desktop, дождитесь зелёного статуса "Engine running" и запустите этот файл снова.
  pause
  exit /b 1
)
if not exist .env copy /y .env.example .env >nul
echo Сборка и запуск сервиса и туннеля. Первый раз это занимает 3-8 минут.
docker compose --profile public up --build -d
if errorlevel 1 (
  echo.
  echo Запуск не удался. Пришлите Claude текст ошибки выше.
  pause
  exit /b 1
)
echo.
echo Ждём сервер и публичную ссылку...
set "URL="
set /a tries=0
:wait
set /a tries+=1
if %tries% gtr 60 (
  echo Ссылка не появилась за 3 минуты. Логи: docker compose logs public-link
  pause
  exit /b 1
)
timeout /t 3 /nobreak >nul
curl -fsS http://127.0.0.1:8080/health >nul 2>&1 || goto wait
for /f "usebackq delims=" %%u in (`powershell -NoProfile -Command "$m = docker compose logs public-link 2>$null | Select-String -Pattern 'https://[a-z0-9-]+\.trycloudflare\.com' | Select-Object -Last 1; if ($m) { $m.Matches[0].Value }"`) do set "URL=%%u"
if not defined URL goto wait
echo.
echo Публичная ссылка: %URL%
echo Её можно открыть с любого устройства, пока этот компьютер и Docker включены.
echo После перезапуска ссылка меняется. Остановить: stop-demo.cmd
start "" %URL%
echo.
pause
