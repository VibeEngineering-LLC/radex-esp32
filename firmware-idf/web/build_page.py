#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import sys
import os
import argparse
import re

sys.stdout.reconfigure(encoding='utf-8')

# Порядок подключения стилей задан списком в коде, НЕ сортировкой каталога:
# это важно для CSS, где порядок правил определяет, какое победит.
STYLES = ["styles/base.css", "styles/radex.css"]

def read_file(path):
    """Читает файл и возвращает его содержимое."""
    with open(path, 'r', encoding='utf-8') as f:
        return f.read()

def write_file(path, content):
    """Записывает содержимое в файл."""
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()

    # Определяем путь к скрипту и рабочую директорию
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)

    # Проверяем наличие шаблона
    template_path = 'index.src.html'
    if not os.path.exists(template_path):
        print("Ошибка: не найден файл index.src.html")
        sys.exit(1)

    try:
        template_content = read_file(template_path)
    except Exception as e:
        print(f"Ошибка чтения шаблона: {e}")
        sys.exit(1)

    # Проверяем наличие метки <!--STYLES-->
    if '<!--STYLES-->' not in template_content:
        print("Ошибка: в шаблоне index.src.html отсутствует метка <!--STYLES-->")
        sys.exit(1)

    # Собираем стили
    style_content = ''
    for style_path in STYLES:
        if not os.path.exists(style_path):
            print(f"Ошибка: не найден файл стиля {style_path}")
            sys.exit(1)
        try:
            style_text = read_file(style_path)
        except Exception as e:
            print(f"Ошибка чтения стиля {style_path}: {e}")
            sys.exit(1)
        style_content += f'/* ===== {style_path} ===== */\n{style_text}\n\n'

    # Вставляем стили в шаблон
    result = template_content.replace('<!--STYLES-->', style_content)

    if args.check:
        # Проверяем, совпадает ли результат с существующим index.html
        output_path = 'index.html'
        if not os.path.exists(output_path):
            print("ВНИМАНИЕ: index.html не существует — сборка необходима")
            sys.exit(2)
        try:
            existing_content = read_file(output_path)
        except Exception as e:
            print(f"Ошибка чтения существующего index.html: {e}")
            sys.exit(1)
        if result == existing_content:
            print("index.html собран из текущих исходников")
            sys.exit(0)
        else:
            print("ВНИМАНИЕ: index.html не совпадает со сборкой из исходников — правка сделана мимо шаблона")
            sys.exit(2)
    else:
        # Записываем результат
        output_path = 'index.html'
        try:
            write_file(output_path, result)
        except Exception as e:
            print(f"Ошибка записи index.html: {e}")
            sys.exit(1)

        # Печатаем статистику
        bytes_count = len(result.encode('utf-8'))
        style_files_count = len(STYLES)
        print(f"Собрано {bytes_count} байт из {style_files_count} файлов стилей")

if __name__ == '__main__':
    main()
