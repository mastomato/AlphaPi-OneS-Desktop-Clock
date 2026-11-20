@echo off
rem 在本目录起一个本地 http 服务并打开改好的网页刷机页。
rem 必须走 http://（不能用 file://），否则浏览器既不给 Web Serial 权限，也不允许自动读取同目录固件文件。
setlocal
cd /d "%~dp0"

where py >nul 2>nul
if %errorlevel% equ 0 (
  start "" "http://localhost:8123/alphapi-one-s-web-flasher.html"
  py -3 -m http.server 8123 --bind 127.0.0.1
  exit /b
)

where python >nul 2>nul
if %errorlevel% equ 0 (
  start "" "http://localhost:8123/alphapi-one-s-web-flasher.html"
  python -m http.server 8123 --bind 127.0.0.1
  exit /b
)

echo Python not found. Install Python 3, then run this file again.
pause
