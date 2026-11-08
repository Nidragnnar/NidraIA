@echo off
rem Abre o instalador do NidraIA (baixa o motor e os modelos)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0instalador\instalar.ps1"
