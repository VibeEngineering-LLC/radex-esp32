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
# Страница собирается из web/index.src.html и web/styles/*.css скриптом
# build_page.py; готовый web/index.html — продукт сборки, в репозитории он не
# хранится (иначе каждая правка стиля давала бы конфликт в 370 КБ). CMake его
# встраивает через EMBED_FILES, поэтому генерация обязана идти ДО idf.py build:
# без неё свежий клон репозитория не собирается вовсе.
if [ -f web/build_page.py ]; then
  (cd web && python3 build_page.py) >> "$LOG" 2>&1 || { echo "BUILD_FAILED: не собралась страница"; tail -5 "$LOG"; exit 1; }
fi
# Локальный режим (W-063): по умолчанию ВЫКЛЮЧЕН, то есть сетевые данные
# в образ не попадают. Значение передаётся в CMake ЯВНО при каждой сборке —
# ноль тоже: иначе единица осталась бы в кэше CMakeCache.txt и следующая
# «обычная» сборка молча собралась бы с кредами.
LOCAL_WIFI="${RADEX_GW_LOCAL_WIFI:-0}"
rm -f /project/build/LOCAL_BUILD_DO_NOT_PUBLISH
if [ "$LOCAL_WIFI" != "0" ]; then
  echo "ВНИМАНИЕ: локальная сборка с сетевыми данными — ПУБЛИКАЦИИ НЕ ПОДЛЕЖИТ"
fi
if idf.py -DRADEX_GW_LOCAL_WIFI="$LOCAL_WIFI" build >> "$LOG" 2>&1; then
  if [ "$LOCAL_WIFI" != "0" ]; then
    echo "локальная сборка: в образе реальная сеть, публиковать нельзя" \
      > /project/build/LOCAL_BUILD_DO_NOT_PUBLISH
  fi
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
