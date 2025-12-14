cmake_minimum_required(VERSION 3.10)
project(SecretSanta)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Исходные файлы
set(SOURCES
    main.cpp
)

# Заголовочные файлы (header-only библиотеки)
set(HEADERS
    httplib.h
    json.hpp
)

# Находим SQLite3
find_library(SQLITE3_LIB
    NAMES sqlite3
    PATHS
        /usr/lib
        /usr/local/lib
        /opt/homebrew/lib
)

# Если не найдено, пробуем через pkg-config
if(NOT SQLITE3_LIB)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(SQLITE3 sqlite3)
    endif()
endif()

# Создаём исполняемый файл
add_executable(santa ${SOURCES} ${HEADERS})

# Подключаем SQLite3
if(SQLITE3_LIB)
    target_link_libraries(santa ${SQLITE3_LIB})
elseif(SQLITE3_FOUND)
    target_link_libraries(santa ${SQLITE3_LIBRARIES})
    target_include_directories(santa PRIVATE ${SQLITE3_INCLUDE_DIRS})
else()
    # Пробуем стандартный путь для macOS
    target_link_libraries(santa sqlite3)
endif()

# Флаги компиляции
target_compile_options(santa PRIVATE -Wall -Wextra)


