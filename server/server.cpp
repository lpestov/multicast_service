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

#define PORT 8080
#define TIMEOUT 10  // Увеличено с 5 до 10 секунд
#define GAME_TIMEOUT 15  // Увеличено время ожидания выбора

enum GameChoice { ROCK, PAPER, SCISSORS, INVALID };
std::unordered_map<std::string, GameChoice> current_choices;
std::mutex clients_mutex, game_mutex;
std::condition_variable cv;
bool game_running = false;

struct ClientInfo {
    std::string name;
    std::string hardware;
    time_t last_seen;
    bool active;
};

std::unordered_map<std::string, ClientInfo> clients;
int server_socket;

void handle_signal(int sig) {
    std::cout << "\nЗавершение работы сервера...\n";
    close(server_socket);
    exit(0);
}

void update_clients() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            time_t now = time(nullptr);
            for (auto& [addr, client] : clients) {
                client.active = (now - client.last_seen) <= TIMEOUT;  // Добавлено <=
            }
        }
        sleep(3);
    }
}

GameChoice string_to_choice(const std::string& s) {
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

void send_to_all(const std::string& message) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (const auto& [addr, client] : clients) {
        if (!client.active) continue;

        size_t colon = addr.find(':');
        std::string ip = addr.substr(0, colon);
        int port = std::stoi(addr.substr(colon + 1));

        sockaddr_in client_addr{};
        client_addr.sin_family = AF_INET;
        client_addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &client_addr.sin_addr);

        sendto(server_socket, message.c_str(), message.size(), 0,
               (sockaddr*)&client_addr, sizeof(client_addr));
    }
}

void determine_winner(std::vector<std::string>& participants) {
    std::unordered_map<GameChoice, std::vector<std::string>> choice_map;
    for (const auto& addr : participants) {
        if (current_choices.count(addr)) {
            choice_map[current_choices[addr]].push_back(addr);
        }
    }

    bool rock = choice_map.count(ROCK);
    bool paper = choice_map.count(PAPER);
    bool scissors = choice_map.count(SCISSORS);

    std::vector<std::string> winners;
    if ((rock && paper && scissors) || (rock && !paper && !scissors) ||
        (!rock && paper && !scissors) || (!rock && !paper && scissors)) {
        send_to_all("НИЧЬЯ! Новый раунд...");
        return;
    }

    if (rock && scissors) {
        winners = choice_map[ROCK];
        send_to_all("Камень бьет ножницы!");
    } else if (paper && rock) {
        winners = choice_map[PAPER];
        send_to_all("Бумага покрывает камень!");
    } else if (scissors && paper) {
        winners = choice_map[SCISSORS];
        send_to_all("Ножницы режут бумагу!");
    }

    participants = winners;
}

void game_round(std::vector<std::string>& participants) {
    current_choices.clear();
    send_to_all("CHOOSE");

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(GAME_TIMEOUT)) {
        std::unique_lock<std::mutex> lock(game_mutex);
        if (current_choices.size() == participants.size()) break;
        lock.unlock();
        usleep(100000);  // Добавлена небольшая задержка для снижения нагрузки на CPU
    }

    determine_winner(participants);
}

void start_game() {
    std::vector<std::string> participants;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (const auto& [addr, client] : clients) {
            if (client.active) participants.push_back(addr);
        }
    }

    if (participants.size() < 2) {
        std::cout << "Для игры нужно минимум 2 активных участника! Сейчас: "
                  << participants.size() << "\n";
        return;
    }

    send_to_all("ИГРА НАЧИНАЕТСЯ! Участников: " + std::to_string(participants.size()));
    while (participants.size() > 1) {
        game_round(participants);
        sleep(1);
    }

    if (!participants.empty()) {
        send_to_all("ПОБЕДИТЕЛЬ: " + participants[0] + "!!!");
    }
}

void handle_commands() {
    std::string cmd;
    while (true) {
        std::cout << "\nКоманды:\n1 - Запросить железо\n2 - Список имен\n3 - Начать игру\n4 - Отключить клиентов\n5 - Список клиентов (статус)\n6 - Выход\n> ";
        std::getline(std::cin, cmd);

        if (cmd == "1") {
            std::lock_guard<std::mutex> lock(clients_mutex);
            for (const auto& [addr, client] : clients) {
                std::cout << client.name << ": " << client.hardware << std::endl;
            }
        } else if (cmd == "2") {
            std::lock_guard<std::mutex> lock(clients_mutex);
            for (const auto& [addr, client] : clients) {
                std::cout << client.name << (client.active ? " (активен)" : " (неактивен)") << std::endl;
            }
        } else if (cmd == "3") {
            std::thread(start_game).detach();
        } else if (cmd == "4") {
            send_to_all("SHUTDOWN");
        } else if (cmd == "5") {
            std::lock_guard<std::mutex> lock(clients_mutex);
            std::cout << "\nСписок клиентов:\n";
            std::cout << "--------------------------------\n";
            for (const auto& [addr, client] : clients) {
                std::cout << "Адрес: " << addr << "\nИмя: " << client.name
                          << "\nЖелезо: " << client.hardware
                          << "\nСтатус: " << (client.active ? "Активен" : "Неактивен")
                          << "\n--------------------------------\n";
            }
        } else if (cmd == "6") {
            handle_signal(0);
        }
    }
}

int main() {
    signal(SIGINT, handle_signal);

    server_socket = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr));

    std::thread(update_clients).detach();
    std::thread(handle_commands).detach();

    char buffer[1024];
    sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);

    while (true) {
        ssize_t len = recvfrom(server_socket, buffer, sizeof(buffer), 0,
                               (sockaddr*)&client_addr, &addr_len);
        if (len <= 0) continue;

        std::string msg(buffer, len);
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        std::string addr = std::string(ip) + ":" + std::to_string(ntohs(client_addr.sin_port));

        if (msg.find("REGISTER:") == 0) {
            size_t pos = msg.find(':', 9);
            ClientInfo info{
                msg.substr(9, pos - 9),
                msg.substr(pos + 1),
                time(nullptr),
                true
            };
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients[addr] = info;
            std::cout << "Зарегистрирован клиент: " << info.name << " (" << addr << ")\n";
        }
        else if (msg == "PING") {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (clients.count(addr)) {
                clients[addr].last_seen = time(nullptr);
            }
        }
        else if (msg == "ROCK" || msg == "PAPER" || msg == "SCISSORS") {
            std::lock_guard<std::mutex> lock(game_mutex);
            current_choices[addr] = string_to_choice(msg);
        }
    }
}
