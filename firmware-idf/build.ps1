# Обёртка: собирает build.sh в Docker (путь проекта с кириллицей, поэтому
# PowerShell, а не Git Bash — тот подменяет /project).
#
# Дефолт — РЕЛИЗНАЯ сборка: сетевых данных в образе нет, конфигурация сети
# вносится порталом RadexGW-Setup или через Web UI (W-063).
#
# Локальная сборка «для себя» (плата сама подключится к моей сети, портал
# проходить не надо):
#     .\build.ps1 -LocalWifi
# Такой образ помечен маркером RADEX-GW-LOCAL-BUILD-DO-NOT-PUBLISH в .rodata,
# рядом появляется файл build\LOCAL_BUILD_DO_NOT_PUBLISH, и гейт
# scripts\check_binary_secrets.py его отвергает. Публикации не подлежит.
param(
    [switch]$LocalWifi
)

$p = Split-Path -Parent $MyInvocation.MyCommand.Path
$local = if ($LocalWifi) { "1" } else { "0" }
if ($LocalWifi) {
    Write-Host "ЛОКАЛЬНАЯ сборка: в образ попадут сетевые данные. НЕ ПУБЛИКОВАТЬ." -ForegroundColor Yellow
}
docker run --rm -v "${p}:/project" -w /project -e RADEX_GW_LOCAL_WIFI=$local `
    espressif/idf:v6.1 bash /project/build.sh
