#!/bin/bash

# Скрипт для сборки проекта Secret Santa

echo "🔨 Сборка проекта Secret Santa..."

# Проверяем наличие компилятора
if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
    echo "❌ Ошибка: не найден компилятор C++"
    exit 1
fi

# Определяем компилятор
if command -v g++ &> /dev/null; then
    CXX=g++
else
    CXX=clang++
fi

# Удаляем старый исполняемый файл
if [ -f "santa" ]; then
    echo "🗑️  Удаление старого исполняемого файла..."
    rm santa
fi

# Компилируем
echo "📦 Компиляция..."
$CXX -std=c++17 -Wall -Wextra -O2 \
    main.cpp \
    -o santa \
    -lsqlite3 \
    -pthread

if [ $? -eq 0 ]; then
    echo "✅ Сборка успешна! Исполняемый файл: ./santa"
    echo ""
    echo "🚀 Для запуска выполните:"
    echo "   ./santa"
else
    echo "❌ Ошибка компиляции"
    exit 1
fi


