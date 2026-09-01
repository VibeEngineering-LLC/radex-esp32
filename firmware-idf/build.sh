#!/bin/sh
# build.sh — петля сборки radex-idf в Docker с КОМПАКТНЫМ отчётом об ошибках.
# Смысл: idf.py при ошибке линковки печатает километровую команду ld (десятки тысяч
# символов). Тащить это в контекст нельзя — скрипт выжимает только суть.
# Запуск (из PowerShell, путь с кириллицей): см. build.ps1 рядом.
set -e
LOG=/project/build_last.log
: > "$LOG"
. /opt/esp/idf/export.sh > /dev/null 2>&1
cd /project
# Целевой чип: переменная окружения RADEX_TARGET (esp32s3 | esp32c3), дефолт esp32s3.
# Смена цели требует удалить sdkconfig — он привязан к чипу (см. sdkconfig.s3.bak).
TARGET="${RADEX_TARGET:-esp32s3}"
if [ ! -f sdkconfig ]; then idf.py set-target "$TARGET" >> "$LOG" 2>&1; fi
if idf.py build >> "$LOG" 2>&1; then
  echo "BUILD_OK"
  grep -E "Project build complete|Successfully created|radex-idf.bin binary size" "$LOG" | tail -3
  exit 0
fi
echo "BUILD_FAILED"
echo "--- compile errors ---"
grep -E "error:|Error |FAILED:" "$LOG" | grep -v "^ccache" | head -20
echo "--- undefined refs (уникальные символы) ---"
grep -oE "undefined reference to \`[^']+'" "$LOG" | sort -u | head -20
echo "--- context ---"
grep -B2 -A4 "error:" "$LOG" | grep -vE "^\s*ccache|^:|-I/opt|-L/opt" | head -30
exit 1
