#include <iostream>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <ctime>
#include <csignal>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <algorithm>
#include <random>
#include <condition_variable>
#include <cstring>
#include <iomanip>
#include <atomic>

#define PORT 8080
#define TIMEOUT 10
#define GAME_TIMEOUT 15

enum GameChoice { ROCK, PAPER, SCISSORS, INVALID };

std::unordered_map<std::string, GameChoice> current_choices;
std::mutex clients_mutex, game_mutex;
bool game_running = false;

struct ClientInfo {
    std::string name;
    std::string hardware;
    time_t last_seen;
    bool active;
};

std::unordered_map<std::string, ClientInfo> clients;
int server_socket;
std::atomic<bool> server_running = true;

void handle_signal(int sig) {
    std::cout << "\n[Server] Получен сигнал " << sig << ", инициируем завершение работы сервера..." << std::endl;
    server_running = false;
    shutdown(server_socket, SHUT_RDWR);
}

void update_clients() {
    std::cout << "[Update Thread] Поток проверки активности запущен (проверки каждые ~3 сек)." << std::endl;
    while (server_running) {
        for (int i = 0; i < 30 && server_running; ++i) {
            usleep(100000);
        }
        if (!server_running) break; {
            std::lock_guard<std::mutex> lock(clients_mutex);
            time_t now = time(nullptr);
            // std::cout << "[Update Thread] Проверка активности клиентов..." << std::endl;
            int became_inactive_count = 0;
            for (auto &[addr, client]: clients) {
                bool was_active = client.active;
                client.active = (now - client.last_seen) <= TIMEOUT;
                if (was_active && !client.active) {
                    std::cout << "[Update Thread] Клиент " << client.name << " (" << addr <<
                            ") стал НЕАКТИВНЫМ (таймаут)." << std::endl;
                    became_inactive_count++;
                }
            }
            // std::cout << "[Update Thread] Проверка завершена." << std::endl;
        }
    }
    // std::cout << "[Update Thread] Поток проверки активности завершен." << std::endl;
}

GameChoice string_to_choice(const std::string &s) {
    if (s == "ROCK") return ROCK;
    if (s == "PAPER") return PAPER;
    if (s == "SCISSORS") return SCISSORS;
    return INVALID;
}

std::string choice_to_string(GameChoice c) {
    switch (c) {
        case ROCK: return "Камень";
        case PAPER: return "Бумага";
        case SCISSORS: return "Ножницы";
        default: return "Неизвестно";
    }
}

void send_to_all_active(const std::string &message) {
    std::cout << "[Send All Active] Отправка сообщения всем активным: \"" << message << "\"" << std::endl;
    std::lock_guard<std::mutex> lock(clients_mutex);
    int sent_count = 0;
    for (const auto &[addr, client]: clients) {
        if (!client.active) continue;

        size_t colon = addr.find(':');
        if (colon == std::string::npos) { continue; }
        std::string ip = addr.substr(0, colon);
        int port = 0;
        try { port = std::stoi(addr.substr(colon + 1)); } catch (...) { continue; }

        sockaddr_in client_addr{};
        memset(&client_addr, 0, sizeof(client_addr));
        client_addr.sin_family = AF_INET;
        client_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &client_addr.sin_addr) <= 0) { continue; }

        ssize_t bytes_sent = sendto(server_socket, message.c_str(), message.size(), 0,
                                    (sockaddr *) &client_addr, sizeof(client_addr));
        if (bytes_sent < 0) {
            std::cerr << "[Send All Active] Ошибка отправки клиенту " << addr << " (errno: " << errno << ")" <<
                    std::endl;
        } else {
            sent_count++;
        }
    }
    // std::cout << "[Send All Active] Сообщение отправлено " << sent_count << " активным клиентам." << std::endl;
}

void send_to_participants(const std::string &message, const std::vector<std::string> &participants) {
    // std::cout << "[Send Participants] Отправка сообщения активным участникам раунда: \"" << message << "\"" << std::endl;
    std::lock_guard<std::mutex> lock(clients_mutex);
    int sent_count = 0;
    for (const auto &addr: participants) {
        if (!clients.count(addr) || !clients[addr].active) {
            continue;
        }

        size_t colon = addr.find(':');
        if (colon == std::string::npos) { continue; }
        std::string ip = addr.substr(0, colon);
        int port = 0;
        try { port = std::stoi(addr.substr(colon + 1)); } catch (...) { continue; }

        sockaddr_in client_addr{};
        memset(&client_addr, 0, sizeof(client_addr));
        client_addr.sin_family = AF_INET;
        client_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &client_addr.sin_addr) <= 0) { continue; }

        ssize_t bytes_sent = sendto(server_socket, message.c_str(), message.size(), 0,
                                    (sockaddr *) &client_addr, sizeof(client_addr));
        if (bytes_sent < 0) {
            // std::cerr << "[Send Participants] Ошибка отправки клиенту " << addr << " (errno: " << errno << ")" << std::endl;
        } else {
            sent_count++;
        }
    }
    // std::cout << "[Send Participants] Сообщение отправлено " << sent_count << " активным участникам раунда." << std::endl;
}


void determine_winner(std::vector<std::string> &participants) {
    std::cout << "[Game Logic] Определение победителя раунда..." << std::endl;
    std::unordered_map<GameChoice, std::vector<std::string> > choice_map;
    std::string choices_log = "Выборы раунда (от активных): ";
    std::vector<std::string> actual_participants_this_round; {
        std::lock_guard<std::mutex> game_lock(game_mutex);
        std::lock_guard<std::mutex> client_lock(clients_mutex);
        for (const auto &addr: participants) {
            if (!clients.count(addr) || !clients[addr].active) {
                continue;
            }
            if (current_choices.count(addr)) {
                GameChoice choice = current_choices[addr];
                if (choice != INVALID) {
                    choice_map[choice].push_back(addr);
                    choices_log += clients[addr].name + "->" + choice_to_string(choice) + "; ";
                    actual_participants_this_round.push_back(addr);
                } else {
                    std::cout << "[Game Logic] Активный участник " << clients[addr].name << " (" << addr <<
                            ") сделал невалидный выбор." << std::endl;
                }
            } else {
                std::cout << "[Game Logic] Активный участник " << clients[addr].name << " (" << addr <<
                        ") не сделал выбор." << std::endl;
            }
        }
    }
    std::cout << "[Game Logic] " << choices_log << std::endl;

    if (actual_participants_this_round.empty()) {
        std::cout << "[Game Logic] Никто из активных участников раунда не сделал валидный выбор." << std::endl;
        send_to_all_active("НИКТО НЕ СДЕЛАЛ ВЫБОР! Ничья. Новый раунд...");
        participants.clear();
        return;
    }

    bool rock = choice_map.count(ROCK);
    bool paper = choice_map.count(PAPER);
    bool scissors = choice_map.count(SCISSORS);
    int valid_choices_type_count = (rock ? 1 : 0) + (paper ? 1 : 0) + (scissors ? 1 : 0);

    std::vector<std::string> winners;
    std::string round_result_msg;

    if (valid_choices_type_count == 3 || valid_choices_type_count == 1) {
        round_result_msg = "НИЧЬЯ! Новый раунд...";
        participants = actual_participants_this_round;
    } else if (valid_choices_type_count == 2) {
        if (rock && scissors) {
            winners = choice_map[ROCK];
            round_result_msg = "Камень бьет ножницы!";
        } else if (paper && rock) {
            winners = choice_map[PAPER];
            round_result_msg = "Бумага покрывает камень!";
        } else if (scissors && paper) {
            winners = choice_map[SCISSORS];
            round_result_msg = "Ножницы режут бумагу!";
        } else {
            round_result_msg = "НИЧЬЯ (неожиданно)! Новый раунд...";
            participants = actual_participants_this_round;
        }
        participants = winners;
    } else {
        round_result_msg = "НИЧЬЯ (ошибка логики)! Новый раунд...";
        participants = actual_participants_this_round;
    }

    std::cout << "[Game Logic] Результат раунда: " << round_result_msg << ". Следующий раунд с " << participants.size()
            << " участниками." << std::endl;
    send_to_all_active(round_result_msg);
}


void game_round(std::vector<std::string> &participants) {
    if (!server_running) return;
    std::cout << "[Game Round] Начало раунда для " << participants.size() << " участников." << std::endl; {
        std::lock_guard<std::mutex> lock(game_mutex);
        current_choices.clear();
    }

    send_to_participants("CHOOSE", participants);

    std::cout << "[Game Round] Ожидание выборов " << GAME_TIMEOUT << " секунд..." << std::endl;
    auto start_time = std::chrono::steady_clock::now();

    size_t expected_choices_count = 0; {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (const auto &p_addr: participants) {
            if (clients.count(p_addr) && clients[p_addr].active) {
                expected_choices_count++;
            }
        }
    }
    std::cout << "[Game Round] Ожидается выборов от " << expected_choices_count << " активных участников." << std::endl;


    while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(GAME_TIMEOUT)) {
        if (!server_running) return;

        size_t current_choice_count = 0; {
            std::lock_guard<std::mutex> game_lock(game_mutex);
            std::lock_guard<std::mutex> client_lock(clients_mutex);
            for (const auto &p_addr: participants) {
                if (clients.count(p_addr) && clients[p_addr].active &&
                    current_choices.count(p_addr) && current_choices[p_addr] != INVALID) {
                    current_choice_count++;
                }
            }
        }


        if (expected_choices_count > 0 && current_choice_count >= expected_choices_count) {
            // std::cout << "[Game Round] Все " << expected_choices_count << " ожидаемых участников сделали выбор." << std::endl;
            break;
        }
        usleep(100000);
    }

    if (std::chrono::steady_clock::now() - start_time >= std::chrono::seconds(GAME_TIMEOUT)) {
        std::cout << "[Game Round] Время ожидания выборов (" << GAME_TIMEOUT << "с) истекло." << std::endl;
    }

    determine_winner(participants);
}

void start_game() {
    if (!server_running) return;
    std::cout << "[Game Manager] Попытка начать игру..." << std::endl;

    if (game_running) {
        std::cout << "[Game Manager] Игра уже активна." << std::endl;
        return;
    }

    std::vector<std::string> participants;
    int active_clients_count = 0; {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (const auto &[addr, client]: clients) {
            if (client.active) {
                participants.push_back(addr);
                active_clients_count++;
            }
        }
    }

    if (active_clients_count < 2) {
        std::cout << "[Game Manager] Для игры нужно минимум 2 активных участника! Сейчас: "
                << active_clients_count << "\n";
        return;
    }

    std::cout << "[Game Manager] Игра начинается! Активных участников: " << active_clients_count << std::endl;
    send_to_all_active("ИГРА НАЧИНАЕТСЯ! Участников: " + std::to_string(participants.size()));
    game_running = true;

    while (participants.size() > 1 && server_running && game_running) {
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            participants.erase(std::remove_if(participants.begin(), participants.end(),
                                              [&](const std::string &addr) {
                                                  return !clients.count(addr) || !clients[addr].active;
                                              }),
                               participants.end());
        }
        if (participants.size() < 2) {
            std::cout << "[Game Manager] Недостаточно активных участников (" << participants.size() <<
                    ") для продолжения игры." << std::endl;
            if (!participants.empty()) {
                break;
            } else {
                send_to_all_active("Все участники выбыли или стали неактивны!");
                game_running = false;
                std::cout << "[Game Manager] Поток игры завершен (нет активных)." << std::endl;
                std::cout <<
                        "\n[Admin] Команды: \n 1:Железо \n 2:Имена(все) \n 3:Игра \n 4:Откл(активных) \n 5:Статус(все) \n 6:Активные \n 7:Выход > "
                        << std::flush;
                return;
            }
        }

        game_round(participants);
        if (!server_running || !game_running) break;
        if (participants.size() > 1) {
            std::cout << "[Game Manager] Пауза 1 секунду перед следующим раундом..." << std::endl;
            sleep(1);
        }
    }

    if (!server_running) {
        std::cout << "[Game Manager] Игра прервана из-за остановки сервера." << std::endl;
        send_to_all_active("ИГРА ПРЕРВАНА ИЗ-ЗА ОСТАНОВКИ СЕРВЕРА!");
    } else if (!participants.empty()) {
        std::string winner_name = "Неизвестный"; {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (clients.count(participants[0])) { winner_name = clients[participants[0]].name; }
        }
        std::string final_msg = "ИГРА ОКОНЧЕНА! ПОБЕДИТЕЛЬ: " + winner_name + " (" + participants[0] + ")!!!";
        std::cout << "[Game Manager] " << final_msg << std::endl;
        send_to_all_active(final_msg);
    } else {
        std::string final_msg = "ИГРА ОКОНЧЕНА! Победителя нет.";
        std::cout << "[Game Manager] " << final_msg << std::endl;
        send_to_all_active(final_msg);
    }

    game_running = false;
    std::cout << "[Game Manager] Поток игры завершен." << std::endl;
    std::cout <<
            "\n[Admin] Команды: \n 1:Железо \n 2:Имена(все) \n 3:Игра \n 4:Откл(активных) \n 5:Статус(все) \n 6:Активные \n 7:Выход > "
            << std::flush;
}

void handle_commands() {
    std::cout << "[Admin Thread] Поток обработки команд запущен. Введите команду." << std::endl;
    std::string cmd;
    while (server_running) {
        std::cout <<
                "\n[Admin] Команды: \n 1:Железо \n 2:Имена(все) \n 3:Игра \n 4:Откл(активных) \n 5:Статус(все) \n 6:Активные \n 7:Выход > "
                << std::flush;

        if (!std::getline(std::cin, cmd)) {
            if (server_running) {
                std::cerr << "[Admin Thread] Ошибка чтения команды из stdin (EOF или ошибка). Завершение потока." <<
                        std::endl;
            }
            break;
        }
        if (!server_running) break;

        cmd.erase(0, cmd.find_first_not_of(" \t\n\r"));
        cmd.erase(cmd.find_last_not_of(" \t\n\r") + 1);

        if (cmd == "1") {
            std::lock_guard<std::mutex> lock(clients_mutex);
            std::cout << "\n[Admin] Информация об оборудовании клиентов:\n";
            if (clients.empty()) { std::cout << "  <Нет зарегистрированных клиентов>\n"; } else {
                for (const auto &[addr, client]: clients) {
                    std::cout << "  " << client.name << " (" << addr << ", " << (
                        client.active ? "Активен" : "Неактивен") << "): " << client.hardware << std::endl;
                }
            }
        } else if (cmd == "2") {
            std::lock_guard<std::mutex> lock(clients_mutex);
            std::cout << "\n[Admin] Имена клиентов (статус):\n";
            if (clients.empty()) { std::cout << "  <Нет зарегистрированных клиентов>\n"; } else {
                for (const auto &[addr, client]: clients) {
                    std::cout << "  " << client.name << (client.active ? " (активен)" : " (неактивен)") << std::endl;
                }
            }
        } else if (cmd == "3") {
            if (game_running) { std::cout << "[Admin] Игра уже идет." << std::endl; } else {
                std::cout << "[Admin] Запуск игры в отдельном потоке..." << std::endl;
                std::thread(start_game).detach();
            }
        } else if (cmd == "4") {
            std::cout << "[Admin] Отправка команды SHUTDOWN всем АКТИВНЫМ клиентам..." << std::endl;
            send_to_all_active("SHUTDOWN");
        } else if (cmd == "5") {
            std::lock_guard<std::mutex> lock(clients_mutex);
            std::cout << "\n[Admin] Список зарегистрированных клиентов и их статус:\n";
            std::cout << "--------------------------------\n";
            if (clients.empty()) { std::cout << "  <Нет зарегистрированных клиентов>\n"; } else {
                for (const auto &[addr, client]: clients) {
                    std::cout << "  Адрес: " << addr << "\n  Имя: " << client.name
                            << "\n  Железо: " << client.hardware
                            << "\n  Статус: " << (client.active ? "Активен" : "Неактивен")
                            << "\n  Посл. сообщ.: " << std::put_time(std::localtime(&client.last_seen),
                                                                     "%Y-%m-%d %H:%M:%S")
                            << "\n--------------------------------\n";
                }
            }
        } else if (cmd == "6") {
            std::lock_guard<std::mutex> lock(clients_mutex);
            std::cout << "\n[Admin] Список АКТИВНЫХ клиентов:\n";
            int active_count = 0;
            for (const auto &[addr, client]: clients) {
                if (client.active) {
                    std::cout << "  - " << client.name << " (" << addr << ")" << std::endl;
                    active_count++;
                }
            }
            if (active_count == 0) {
                std::cout << "  <Нет активных клиентов>\n";
            } else {
                std::cout << "  Всего активных: " << active_count << std::endl;
            }
        } else if (cmd == "7") {
            std::cout << "[Admin] Команда на выход, инициируем остановку сервера..." << std::endl;
            handle_signal(0);
        } else {
            std::cout << "[Admin] Неизвестная команда." << std::endl;
        }
    }
    std::cout << "[Admin Thread] Поток обработки команд завершен." << std::endl;
}


int main() {
    std::cout << "[Server Main] Запуск сервера на порту " << PORT << "..." << std::endl;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    server_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_socket < 0) {
        perror("[Server Main] Ошибка создания серверного сокета");
        return 1;
    }
    std::cout << "[Server Main] Серверный сокет создан." << std::endl;

    sockaddr_in server_addr{};
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int reuse = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("[Server Main] setsockopt(SO_REUSEADDR) failed");
    }


    if (bind(server_socket, (sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        perror("[Server Main] Ошибка привязки серверного сокета");
        close(server_socket);
        return 1;
    }
    std::cout << "[Server Main] Серверный сокет привязан к порту " << PORT << "." << std::endl;

    std::thread update_thread(update_clients);
    std::thread command_thread(handle_commands);

    std::cout << "[Server Main] Сервер готов к приему сообщений..." << std::endl;

    char buffer[1024];
    sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);

    while (server_running) {
        memset(&client_addr, 0, sizeof(client_addr));
        addr_len = sizeof(client_addr);

        ssize_t len = recvfrom(server_socket, buffer, sizeof(buffer) - 1, 0,
                               (sockaddr *) &client_addr, &addr_len);

        if (!server_running) break;

        if (len > 0) {
            std::string msg(buffer, len);
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
            std::string addr = std::string(ip) + ":" + std::to_string(ntohs(client_addr.sin_port));

            if (msg.rfind("REGISTER:", 0) == 0) {
                size_t first_colon = 9;
                size_t second_colon = msg.find(':', first_colon);
                if (second_colon != std::string::npos) {
                    std::string reg_name = msg.substr(first_colon, second_colon - first_colon);
                    std::string reg_hardware = msg.substr(second_colon + 1);
                    ClientInfo info{reg_name, reg_hardware, time(nullptr), true}; {
                        std::lock_guard<std::mutex> lock(clients_mutex);
                        bool is_new = clients.find(addr) == clients.end();
                        clients[addr] = info;
                        if (is_new) {
                            std::cout << "[Server Main] Зарегистрирован НОВЫЙ клиент: " << info.name << " (" << addr <<
                                    ")" << std::endl;
                        } else {
                            std::cout << "[Server Main] Обновлен клиент: " << info.name << " (" << addr << ")" <<
                                    std::endl;
                        }
                    }
                } else {
                    std::cerr << "[Server Main] Неверный формат REGISTER от " << addr << ": " << msg << std::endl;
                }
            } else if (msg == "PING") {
                std::lock_guard<std::mutex> lock(clients_mutex);
                if (clients.count(addr)) {
                    clients[addr].last_seen = time(nullptr);
                    if (!clients[addr].active) {
                        std::cout << "[Server Main] Клиент " << clients[addr].name << " (" << addr <<
                                ") снова активен (получен PING)." << std::endl;
                    }
                    clients[addr].active = true;
                } else {
                    std::cout << "[Server Main] Получен PING от НЕИЗВЕСТНОГО клиента " << addr << ". Игнорируется." <<
                            std::endl;
                }
            } else if (msg == "ROCK" || msg == "PAPER" || msg == "SCISSORS") {
                if (game_running) {
                    GameChoice choice = string_to_choice(msg);
                    if (choice != INVALID) {
                        std::lock_guard<std::mutex> clients_lock(clients_mutex);
                        if (clients.count(addr) && clients[addr].active) {
                            // Принимаем только от активных
                            std::lock_guard<std::mutex> game_lock(game_mutex);
                            current_choices[addr] = choice;
                            std::cout << "[Server Main] Активный игрок " << clients[addr].name << " (" << addr <<
                                    ") выбрал: " << msg << std::endl;
                        }
                    }
                }
            } else {
                std::cout << "[Server Main] Получено неизвестное сообщение от " << addr << ": " << msg << std::endl;
            }
        } else if (len < 0) {
            if (!server_running) break;
            if (errno == EINTR) { continue; } else if (errno == EBADF) {
                std::cout << "[Server Main] Серверный сокет закрыт (EBADF)." << std::endl;
                break;
            } else { perror("[Server Main] Ошибка приема recvfrom в основном цикле"); }
        }
    }

    std::cout << "[Server Main] Основной цикл приема сообщений завершен." << std::endl;

    std::cout << "[Server Main] Ожидание завершения фоновых потоков..." << std::endl;
    if (update_thread.joinable()) {
        update_thread.join();
        std::cout << "[Server Main] Поток обновления клиентов завершен." << std::endl;
    }
    if (command_thread.joinable()) {
        command_thread.join();
        std::cout << "[Server Main] Поток команд администратора завершен." << std::endl;
    }

    close(server_socket);
    std::cout << "[Server Main] Сервер завершил работу." << std::endl;
    return 0;
}
