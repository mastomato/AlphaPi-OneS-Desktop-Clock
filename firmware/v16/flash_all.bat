@echo off
chcp 65001 >nul 2>&1
cd /d "%~dp0"
echo.
echo === AlphaPi One S 六段式刷机（小智自定义界面 v3 + 网络收音机）===
echo.
where py >nul 2>&1
if %errorlevel% equ 0 (
  py -3 flash_all.py %*
) else (
  python flash_all.py %*
)
echo.
pause
