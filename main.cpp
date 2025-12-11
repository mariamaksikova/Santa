#include "httplib.h"
#include "json.hpp"
#include <sqlite3.h>
#include <string>
#include <random>
#include <algorithm>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <mutex>

using json = nlohmann::json;

int main()
{
    httplib::Server server;

    // Инициализируем SQLite
    sqlite3 *db;
    if (sqlite3_open("santa.db", &db) != SQLITE_OK)
    {
        std::cerr << "Ошибка открытия БД" << std::endl;
        return 1;
    }

    // Включаем WAL режим для безопасного многопоточного доступа
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    // Мьютекс для защиты доступа к БД из разных потоков
    std::mutex db_mutex;

    // Создаём таблицы
    const char *create_games = R"(
        CREATE TABLE IF NOT EXISTS games (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            budget INTEGER,
            invite TEXT UNIQUE NOT NULL,
            max_participants INTEGER NOT NULL DEFAULT 10,
            status TEXT NOT NULL DEFAULT 'waiting'
        )
    )";
    const char *create_participants = R"(
        CREATE TABLE IF NOT EXISTS participants (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            game_id INTEGER NOT NULL,
            name TEXT NOT NULL,
            wishes TEXT,
            FOREIGN KEY(game_id) REFERENCES games(id)
        )
    )";
    const char *create_assignments = R"(
        CREATE TABLE IF NOT EXISTS assignments (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            game_id INTEGER NOT NULL,
            giver_id INTEGER NOT NULL,
            receiver_id INTEGER NOT NULL,
            FOREIGN KEY(game_id) REFERENCES games(id),
            FOREIGN KEY(giver_id) REFERENCES participants(id),
            FOREIGN KEY(receiver_id) REFERENCES participants(id)
        )
    )";

    sqlite3_exec(db, create_games, nullptr, nullptr, nullptr);
    sqlite3_exec(db, create_participants, nullptr, nullptr, nullptr);
    sqlite3_exec(db, create_assignments, nullptr, nullptr, nullptr);

    // Миграция: добавляем новые поля, если их нет
    // Проверяем наличие колонок и добавляем их
    sqlite3_stmt *check_stmt = nullptr;
    const char *check_sql = "PRAGMA table_info(games)";

    if (sqlite3_prepare_v2(db, check_sql, -1, &check_stmt, nullptr) == SQLITE_OK && check_stmt != nullptr)
    {
        bool has_max_participants = false;
        bool has_status = false;

        while (sqlite3_step(check_stmt) == SQLITE_ROW)
        {
            const char *col_name = reinterpret_cast<const char *>(sqlite3_column_text(check_stmt, 1));
            if (col_name)
            {
                if (strcmp(col_name, "max_participants") == 0)
                {
                    has_max_participants = true;
                }
                if (strcmp(col_name, "status") == 0)
                {
                    has_status = true;
                }
            }
        }
        sqlite3_finalize(check_stmt);
        check_stmt = nullptr;

        // Добавляем недостающие колонки
        char *err_msg = nullptr;
        if (!has_max_participants)
        {
            // Добавляем колонку без NOT NULL, затем обновляем значения
            if (sqlite3_exec(db, "ALTER TABLE games ADD COLUMN max_participants INTEGER DEFAULT 10", nullptr, nullptr, &err_msg) != SQLITE_OK)
            {
                std::cerr << "Ошибка добавления max_participants: " << (err_msg ? err_msg : "неизвестная ошибка") << std::endl;
                if (err_msg)
                    sqlite3_free(err_msg);
            }
            else
            {
                // Обновляем существующие записи
                sqlite3_exec(db, "UPDATE games SET max_participants = 10 WHERE max_participants IS NULL", nullptr, nullptr, nullptr);
            }
        }
        if (!has_status)
        {
            err_msg = nullptr;
            // Добавляем колонку без NOT NULL, затем обновляем значения
            if (sqlite3_exec(db, "ALTER TABLE games ADD COLUMN status TEXT DEFAULT 'waiting'", nullptr, nullptr, &err_msg) != SQLITE_OK)
            {
                std::cerr << "Ошибка добавления status: " << (err_msg ? err_msg : "неизвестная ошибка") << std::endl;
                if (err_msg)
                    sqlite3_free(err_msg);
            }
            else
            {
                // Обновляем существующие записи
                sqlite3_exec(db, "UPDATE games SET status = 'waiting' WHERE status IS NULL", nullptr, nullptr, nullptr);
            }
        }
    }
    else
    {
        std::cerr << "Предупреждение: не удалось проверить схему таблицы games" << std::endl;
    }

    // Главная страница
    server.Get("/", [&db_mutex](const httplib::Request &, httplib::Response &res)
               {
        std::ifstream file("templates/index.html");
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            res.set_content(buffer.str(), "text/html");
        } else {
            res.set_content("<h1>Создайте templates/index.html</h1>", "text/html");
        } });

    // Создание игры
    server.Post("/api/create-game", [&db, &db_mutex](const httplib::Request &req, httplib::Response &res)
                {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            auto body = json::parse(req.body);
            std::string title = body.value("title", "Тайный Санта");
            int budget = body.value("budget", 3000);
            int max_participants = body.value("max_participants", 10);

            if (max_participants < 2) {
                res.status = 400;
                res.set_content("{\"error\": \"Минимум 2 участника\"}", "application/json");
                return;
            }

            // Генерируем уникальный код
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(100000, 999999);
            std::string invite = std::to_string(dis(gen));

            // Сохраняем в БД
            std::string sql = "INSERT INTO games (title, budget, invite, max_participants) VALUES (?, ?, ?, ?)";
            sqlite3_stmt* stmt;
            sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
            sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 2, budget);
            sqlite3_bind_text(stmt, 3, invite.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 4, max_participants);
            
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                int game_id = sqlite3_last_insert_rowid(db);
                json response = {
                    {"success", true},
                    {"invite_link", "/join/" + invite},
                    {"room_link", "/room/" + invite},
                    {"title", title},
                    {"game_id", game_id}
                };
                res.set_content(response.dump(), "application/json");
            } else {
                res.status = 500;
                res.set_content("{\"error\": \"Ошибка создания игры\"}", "application/json");
            }
            sqlite3_finalize(stmt);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content("{\"error\": \"Неверный запрос\"}", "application/json");
        } });

    // Страница присоединения к игре
    server.Get(R"(/join/(\d+))", [&db, &db_mutex](const httplib::Request &req, httplib::Response &res)
               {
        std::lock_guard<std::mutex> lock(db_mutex);
        std::string code = req.matches[1].str();
        
        // Проверяем, существует ли игра
        std::string sql = "SELECT id, title FROM games WHERE invite = ?";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_STATIC);
        
        bool game_exists = false;
        int game_id = 0;
        std::string title;
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            game_exists = true;
            game_id = sqlite3_column_int(stmt, 0);
            title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        }
        sqlite3_finalize(stmt);

        if (!game_exists) {
            res.status = 404;
            res.set_content("<h1>Игра не найдена</h1>", "text/html");
            return;
        }

        // Отдаём HTML с подстановкой
        std::ifstream file("templates/join.html");
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string html = buffer.str();
            
            // Заменяем плейсхолдеры
            size_t pos = html.find("{{CODE}}");
            if (pos != std::string::npos) {
                html.replace(pos, 8, code);
            }
            pos = html.find("{{TITLE}}");
            if (pos != std::string::npos) {
                html.replace(pos, 9, title);
            }
            
            res.set_content(html, "text/html");
        } else {
            res.set_content("<h1>Создайте templates/join.html</h1>", "text/html");
        } });

    // Добавление участника
    server.Post("/api/join", [&db, &db_mutex](const httplib::Request &req, httplib::Response &res)
                {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            auto body = json::parse(req.body);
            std::string code = body["code"];
            std::string name = body.value("name", "");
            std::string wishes = body.value("wishes", "");

            if (name.empty()) {
                res.status = 400;
                res.set_content("{\"error\": \"Имя обязательно\"}", "application/json");
                return;
            }

            // Находим игру
            std::string sql = "SELECT id FROM games WHERE invite = ?";
            sqlite3_stmt* stmt;
            sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
            sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_STATIC);
            
            if (sqlite3_step(stmt) != SQLITE_ROW) {
                res.status = 404;
                res.set_content("{\"error\": \"Игра не найдена\"}", "application/json");
                sqlite3_finalize(stmt);
                return;
            }

            int game_id = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);

            // Проверяем количество участников
            sql = "SELECT COUNT(*) FROM participants WHERE game_id = ?";
            sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, game_id);
            int current_count = 0;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                current_count = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);

            // Проверяем лимит
            sql = "SELECT max_participants, status FROM games WHERE id = ?";
            sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, game_id);
            int max_participants = 10;
            std::string status = "waiting";
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                max_participants = sqlite3_column_int(stmt, 0);
                const char* status_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (status_ptr) status = status_ptr;
            }
            sqlite3_finalize(stmt);

            if (status == "started") {
                res.status = 400;
                res.set_content("{\"error\": \"Игра уже началась\"}", "application/json");
                return;
            }

            if (current_count >= max_participants) {
                res.status = 400;
                res.set_content("{\"error\": \"Комната заполнена\"}", "application/json");
                return;
            }

            // Добавляем участника
            sql = "INSERT INTO participants (game_id, name, wishes) VALUES (?, ?, ?)";
            sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, game_id);
            sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, wishes.c_str(), -1, SQLITE_STATIC);
            
            int participant_id = 0;
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                participant_id = sqlite3_last_insert_rowid(db);
            } else {
                res.status = 500;
                res.set_content("{\"error\": \"Ошибка добавления\"}", "application/json");
                sqlite3_finalize(stmt);
                return;
            }
            sqlite3_finalize(stmt);

            // Проверяем, достигли ли лимита
            current_count++;
            bool should_start = (current_count >= max_participants);

            if (should_start) {
                // Запускаем рандомайзер
                sql = "SELECT id FROM participants WHERE game_id = ? ORDER BY id";
                sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
                sqlite3_bind_int(stmt, 1, game_id);
                
                std::vector<int> participant_ids;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    participant_ids.push_back(sqlite3_column_int(stmt, 0));
                }
                sqlite3_finalize(stmt);

                // Создаём случайные пары (каждый дарит следующему по кругу)
                std::random_device rd;
                std::mt19937 gen(rd());
                std::shuffle(participant_ids.begin(), participant_ids.end(), gen);

                // Создаём assignments
                for (size_t i = 0; i < participant_ids.size(); i++) {
                    int giver_id = participant_ids[i];
                    int receiver_id = participant_ids[(i + 1) % participant_ids.size()];
                    
                    sql = "INSERT INTO assignments (game_id, giver_id, receiver_id) VALUES (?, ?, ?)";
                    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
                    sqlite3_bind_int(stmt, 1, game_id);
                    sqlite3_bind_int(stmt, 2, giver_id);
                    sqlite3_bind_int(stmt, 3, receiver_id);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }

                // Обновляем статус игры
                sql = "UPDATE games SET status = 'started' WHERE id = ?";
                sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
                sqlite3_bind_int(stmt, 1, game_id);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }

            json response = {
                {"success", true},
                {"message", "Ты успешно присоединился к игре!"},
                {"name", name},
                {"participant_id", participant_id},
                {"room_link", "/room/" + code},
                {"game_started", should_start}
            };
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content("{\"error\": \"Неверный запрос\"}", "application/json");
        } });

    // Страница комнаты
    server.Get(R"(/room/(\d+))", [&db, &db_mutex](const httplib::Request &req, httplib::Response &res)
               {
        std::lock_guard<std::mutex> lock(db_mutex);
        std::string code = req.matches[1].str();
        
        // Проверяем, существует ли игра
        std::string sql = "SELECT id, title, status FROM games WHERE invite = ?";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_STATIC);
        
        bool game_exists = false;
        int game_id = 0;
        std::string title;
        std::string status = "waiting";
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            game_exists = true;
            game_id = sqlite3_column_int(stmt, 0);
            title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* status_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            if (status_ptr) status = status_ptr;
        }
        sqlite3_finalize(stmt);

        if (!game_exists) {
            res.status = 404;
            res.set_content("<h1>Игра не найдена</h1>", "text/html");
            return;
        }

        // Отдаём HTML с подстановкой
        std::ifstream file("templates/room.html");
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string html = buffer.str();
            
            // Заменяем плейсхолдеры
            size_t pos = html.find("{{CODE}}");
            if (pos != std::string::npos) {
                html.replace(pos, 8, code);
            }
            pos = html.find("{{TITLE}}");
            if (pos != std::string::npos) {
                html.replace(pos, 9, title);
            }
            pos = html.find("{{STATUS}}");
            if (pos != std::string::npos) {
                html.replace(pos, 10, status);
            }
            
            res.set_content(html, "text/html");
        } else {
            res.set_content("<h1>Создайте templates/room.html</h1>", "text/html");
        } });

    // API: Получение информации о комнате
    server.Get(R"(/api/room/(\d+))", [&db, &db_mutex](const httplib::Request &req, httplib::Response &res)
               {
        std::lock_guard<std::mutex> lock(db_mutex);
        std::string code = req.matches[1].str();
        
        std::string sql = "SELECT id, title, max_participants, status FROM games WHERE invite = ?";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_STATIC);
        
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            res.status = 404;
            res.set_content("{\"error\": \"Игра не найдена\"}", "application/json");
            sqlite3_finalize(stmt);
            return;
        }

        int game_id = sqlite3_column_int(stmt, 0);
        const char* title_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        std::string title = title_ptr ? title_ptr : "";
        int max_participants = sqlite3_column_int(stmt, 2);
        const char* status_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        std::string status = status_ptr ? status_ptr : "waiting";
        sqlite3_finalize(stmt);

        // Получаем список участников
        sql = "SELECT id, name, wishes FROM participants WHERE game_id = ? ORDER BY id";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, game_id);
        
        json participants = json::array();
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* name_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* wishes_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            json participant = {
                {"id", sqlite3_column_int(stmt, 0)},
                {"name", name_ptr ? name_ptr : ""},
                {"wishes", wishes_ptr ? wishes_ptr : ""}
            };
            participants.push_back(participant);
        }
        sqlite3_finalize(stmt);

        json response = {
            {"game_id", game_id},
            {"title", title},
            {"max_participants", max_participants},
            {"current_count", participants.size()},
            {"status", status},
            {"participants", participants}
        };
        res.set_content(response.dump(), "application/json"); });

    // API: Получение назначения для участника
    server.Get(R"(/api/assignment/(\d+)/(\d+))", [&db, &db_mutex](const httplib::Request &req, httplib::Response &res)
               {
        std::lock_guard<std::mutex> lock(db_mutex);
        std::string code = req.matches[1].str();
        int participant_id = std::stoi(req.matches[2].str());
        
        std::string sql = "SELECT id FROM games WHERE invite = ?";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_STATIC);
        
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            res.status = 404;
            res.set_content("{\"error\": \"Игра не найдена\"}", "application/json");
            sqlite3_finalize(stmt);
            return;
        }
        int game_id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        // Получаем назначение
        sql = "SELECT receiver_id FROM assignments WHERE game_id = ? AND giver_id = ?";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, game_id);
        sqlite3_bind_int(stmt, 2, participant_id);
        
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            res.status = 404;
            res.set_content("{\"error\": \"Назначение не найдено\"}", "application/json");
            sqlite3_finalize(stmt);
            return;
        }
        int receiver_id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        // Получаем информацию о получателе
        sql = "SELECT name, wishes FROM participants WHERE id = ?";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, receiver_id);
        
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            res.status = 404;
            res.set_content("{\"error\": \"Участник не найден\"}", "application/json");
            sqlite3_finalize(stmt);
            return;
        }
        const char* receiver_name_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string receiver_name = receiver_name_ptr ? receiver_name_ptr : "";
        const char* wishes_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        std::string wishes = wishes_ptr ? wishes_ptr : "";
        sqlite3_finalize(stmt);

        json response = {
            {"receiver_name", receiver_name},
            {"wishes", wishes}
        };
        res.set_content(response.dump(), "application/json"); });

    // Статические файлы
    server.set_mount_point("/static", "./static");

    std::cout << "🚀 Сервер запущен!" << std::endl;
    std::cout << "   Локально: http://localhost:8080" << std::endl;
    std::cout << "   В сети:   http://<ваш-IP-адрес>:8080" << std::endl;
    std::cout << "   (Узнайте IP: ifconfig | grep 'inet ' | grep -v 127.0.0.1)" << std::endl;
    std::cout << std::flush;

    // Слушаем на всех интерфейсах (0.0.0.0) для доступа по локальной сети
    // listen блокирует выполнение, поэтому sqlite3_close никогда не выполнится
    if (!server.listen("0.0.0.0", 8080))
    {
        std::cerr << "Ошибка: не удалось запустить сервер на порту 8080" << std::endl;
        sqlite3_close(db);
        return 1;
    }

    // Этот код никогда не выполнится, так как listen блокирует
    sqlite3_close(db);

    sqlite3_close(db);
    return 0;
}