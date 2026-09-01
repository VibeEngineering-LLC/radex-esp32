#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Скрипт проверки бинарных файлов на наличие секретов и маркера локальной сборки.

Повод:
Инцидент W-063: релиз v1.0.0 прошивки `radex-gw-idf` содержал образы, в
которые при компиляции попали реальные SSID и пароль Wi-Fi среды сборки.
Образы пролежали в публичном доступе несколько часов.

Проверка, которая тогда применялась (`strings <образ> | grep <секрет>`),
дала ноль совпадений при фактическом наличии секрета в образе: утилиты
`strings` на машине нет вовсе, и конвейер молча вернул пустой результат.
Отсюда главное требование: поиск ведётся ПРЯМЫМ БАЙТОВЫМ сравнением
средствами Python, никаких внешних утилит и никаких конвейеров.

Пример запуска:
python check_binary_secrets.py firmware.bin
python check_binary_secrets.py --secrets my_secrets.txt --quiet fw1.bin fw2.bin
"""

import argparse
import hashlib
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")

def main() -> int:
    parser = argparse.ArgumentParser(description="Проверка бинарных файлов на наличие секретов и маркера локальной сборки")
    parser.add_argument("--secrets", type=str, help="Файл со списком известных секретов (по умолчанию secrets_known.txt)")
    parser.add_argument("--quiet", action="store_true", help="Печатать только итог и находки, без разбора по каждому значению")
    parser.add_argument("images", nargs="+", help="Один или несколько путей к файлам образов")

    args = parser.parse_args()

    if not args.images:
        parser.print_help()
        return 2

    script_dir = Path(__file__).resolve().parent
    # str(), а не Path: load() определяет формат через path.lower(), и Path
    # уронил бы проверку AttributeError'ом — то есть кодом 1 вместо разбора.
    secrets_file = args.secrets or str(script_dir / "secrets_known.txt")

    try:
        sys.path.insert(0, str(script_dir))
        from fw_secrets_common import load, BENIGN
    except ImportError as e:
        print(f"Ошибка импорта: {e}")
        return 2

    try:
        secrets = load(secrets_file)
    except Exception as e:
        print(f"Ошибка загрузки секретов: {e}")
        return 2

    if not secrets:
        print("Список секретов пуст. Проверка не проводилась.")
        return 2

    all_clean = True
    findings = []

    for image_path in args.images:
        try:
            with open(image_path, "rb") as f:
                data = f.read()
        except Exception as e:
            print(f"Ошибка чтения файла {image_path}: {e}")
            return 2

        size = len(data)
        sha256 = hashlib.sha256(data).hexdigest()

        image_findings = {
            "path": image_path,
            "size": size,
            "sha256": sha256,
            "leaks": [],
            "false_positives": [],
            "local_build_marker_found": False
        }

        # Проверка маркера локальной сборки
        local_marker = b"RADEX-GW-LOCAL-BUILD-DO-NOT-PUBLISH"
        if data.find(local_marker) != -1:
            image_findings["local_build_marker_found"] = True
            all_clean = False

        # Поиск секретов
        found_secrets = 0
        for name, value in secrets.items():
            found_secrets += 1
            encodings = [
                (value.encode("utf-8"), "utf-8"),
                (value.encode("utf-16-le"), "utf-16le")
            ]

            for encoded_value, encoding in encodings:
                positions = []
                start = 0
                while True:
                    pos = data.find(encoded_value, start)
                    if pos == -1:
                        break
                    positions.append(pos)
                    start = pos + 1

                if not positions:
                    continue

                count = len(positions)
                offsets = [f"0x{p:08x}" for p in positions[:5]]

                # Контексты вокруг вхождений — БАЙТЫ: элементы BENIGN тоже
                # байтовые, а `b"abc" in "строка"` — это TypeError, не False.
                contexts = [data[max(0, p - 48):p + len(encoded_value) + 48]
                            for p in positions[:5]]
                # Вероятно ложное: значение из ОДНОГО класса символов (только
                # цифры либо только буквы), короче 12 символов, и каждое
                # вхождение сидит внутри технической строки. isalnum() здесь
                # не годится: смешанное значение — это уже пароль.
                single_class = value.isdigit() or value.isalpha()
                is_false_positive = (
                    single_class and len(value) < 12
                    and all(any(b in ctx for b in BENIGN) for ctx in contexts)
                )

                if is_false_positive:
                    image_findings["false_positives"].append({
                        "name": name,
                        "encoding": encoding,
                        "count": count,
                        "offsets": offsets,
                        "context": contexts[0]
                    })
                else:
                    image_findings["leaks"].append({
                        "name": name,
                        "encoding": encoding,
                        "count": count,
                        "offsets": offsets
                    })
                    all_clean = False

        findings.append(image_findings)

    # Вывод отчёта
    total_secrets = len(secrets)
    total_images = len(args.images)

    for image_findings in findings:
        path = image_findings["path"]
        size = image_findings["size"]
        sha256 = image_findings["sha256"]

        print(f"Файл: {path}")
        if not args.quiet:
            print(f"  Размер: {size} байт")
            print(f"  SHA256: {sha256}")

        if image_findings["leaks"]:
            for leak in image_findings["leaks"]:
                print(f"  УТЕЧКА: {leak['name']} ({leak['encoding']}), вхождений {leak['count']}, смещения {', '.join(leak['offsets'])}")

        if image_findings["false_positives"]:
            for fp in image_findings["false_positives"]:
                print(f"  ЛОЖНОЕ СОВПАДЕНИЕ: {fp['name']} ({fp['encoding']}), вхождений {fp['count']}")
                # Контекст берётся сохранённым при обнаружении: пересчитывать
                # его здесь нельзя — `data` в этой точке принадлежит ПОСЛЕДНЕМУ
                # разобранному образу, а находка может быть из первого.
                printable = "".join(chr(b) if 32 <= b < 127 else "."
                                    for b in fp["context"])
                print(f"    Контекст: {printable[:96]}")

        if image_findings["local_build_marker_found"]:
            print("  МАРКЕР ЛОКАЛЬНОЙ СБОРКИ НАЙДЕН — образ собран с реальной "
                  "сетью внутри (RADEX_GW_LOCAL_WIFI=1)")
        elif not args.quiet:
            print("  Маркер локальной сборки: не найден")

        verdict = "ПУБЛИКОВАТЬ НЕЛЬЗЯ" if (image_findings["leaks"] or image_findings["local_build_marker_found"]) else "Чист"
        print(f"  Вердикт по образу: {verdict}")
        print()

    # Общий итог
    if all_clean:
        print(f"ВЕРДИКТ: образы можно публиковать")
        print(f"Совпадений нет (проверено {total_secrets} значений в {total_images} образах)")
    else:
        problematic = [f["path"] for f in findings if f["leaks"] or f["local_build_marker_found"]]
        print(f"ВЕРДИКТ: ПУБЛИКОВАТЬ НЕЛЬЗЯ")
        print("Необходимо исправить следующие файлы:")
        for p in problematic:
            print(f"  {p}")

    return 1 if not all_clean else 0


if __name__ == "__main__":
    sys.exit(main())
