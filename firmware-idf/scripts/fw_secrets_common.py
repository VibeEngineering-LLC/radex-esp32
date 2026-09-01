# -*- coding: utf-8 -*-
"""
Общий разбор файла секретов для проверки прошивок на утечку секретов.

Импортируется двумя скриптами: check_firmware_secrets.py (ESPHome-ветка) и
check_binary_secrets.py (ESP-IDF-ветка). Не должен копироваться, а должен
импортироваться, чтобы избежать расхождения логики между двумя проверками.

Повод — инцидент W-063: релиз v1.0.0 ушёл с образами, в которых лежали
реальные сетевые данные среды сборки. Разбор файла секретов вынесен сюда,
чтобы существовать в одном экземпляре: две копии расходятся молча, и
сильная проверка в одном скрипте не спасает второй. Также учитывается
#RADEX-85: в ESP-IDF-ветке секреты берутся из заголовка `#define`, не из YAML.

Форматы файлов определяются по расширению, а не содержимому — это
предотвращает ошибки при перепутанных расширениях.
"""

import io
import os
import re

BENIGN = [
    b"0123456789abcdef",
    b"0123456789ABCDEF",
    b"abcdefghijklmnopqrstuvwxyz",
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
]


def load(path):
    """
    Загружает секреты из файла по указанному пути.

    :param path: путь к файлу с секретами
    :return: словарь {имя: значение} секретов
    """
    if not path or not os.path.exists(path):
        return {}

    _, ext = os.path.splitext(path)
    is_header = ext.lower() in (".h", ".hpp")

    result = {}
    with io.open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not is_header and line.lstrip().startswith("#"):
                continue  # пропускаем комментарии в non-header файлах

            if is_header:
                match = re.match(r"^\s*#define\s+(\w+)\s+\"(.+?)\"", line)
            else:
                match = re.match(r"^(\w+):\s*\"?([^\"\n]+?)\"?\s*$", line)

            if not match:
                continue

            key, value = match.groups()
            if len(value) < 4:
                continue

            result[key] = value

    return result
