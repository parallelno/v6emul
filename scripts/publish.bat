@echo off
setlocal

for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyy.MM.dd"') do set "TAG=v%%I"

cmake --preset release
git tag %TAG%
git push --follow-tags