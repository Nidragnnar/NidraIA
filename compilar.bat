@echo off
chcp 65001 >nul
rem Compila o GeradorImagens.exe com o CMake do Visual Studio
cd /d "%~dp0"

set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 set "CMAKE=%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

"%CMAKE%" -S . -B build -G "Visual Studio 18 2026" -A x64 || goto erro
"%CMAKE%" --build build --config Release || goto erro
echo.
echo Pronto: GeradorImagens.exe
exit /b 0

:erro
echo.
echo A compilacao falhou. Veja as mensagens acima.
pause
exit /b 1
