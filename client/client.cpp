#include <iostream>
#include <thread>
#include <csignal>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <random>

bool running = true;
int client_socket;
sockaddr_in server_addr{};
std::string client_name;

void signal_handler(int sig) {
    running = false;
    close(client_socket);
    exit(0);
}

std::string get_hardware() {
    struct sysinfo info{};
    sysinfo(&info);
    return "CPU:" + std::to_string(sysconf(_SC_NPROCESSORS_ONLN)) +
           " RAM:" + std::to_string(info.totalram / 1024 / 1024) + "MB";
}

void register_client() {
    std::string msg = "REGISTER:" + client_name + ":" + get_hardware();
    if (sendto(client_socket, msg.c_str(), msg.size(), 0,
               (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Ошибка регистрации");
    }
}

void handle_server_commands() {
    char buffer[1024];
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, 2);
    const std::vector<std::string> options = {"ROCK", "PAPER", "SCISSORS"};

    while (running) {
        sockaddr_in server_addr_tmp{};
        socklen_t addr_len = sizeof(server_addr_tmp);
        ssize_t len = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                               (sockaddr*)&server_addr_tmp, &addr_len);
        if (len <= 0) continue;

        std::string cmd(buffer, len);
        if (cmd == "CHOOSE") {
            std::string choice = options[distrib(gen)];
            sendto(client_socket, choice.c_str(), choice.size(), 0,
                   (sockaddr*)&server_addr, sizeof(server_addr));
        }
        else if (cmd == "SHUTDOWN") {
            std::cout << "Получена команда на отключение\n";
            signal_handler(0);
        }
        else {
            std::cout << "Сообщение от сервера: " << cmd << std::endl;
        }
    }
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    client_name = (argc > 1) ? argv[1] : "Client_" + std::to_string(getpid());

    client_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_socket < 0) {
        perror("Ошибка создания сокета");
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "server", &server_addr.sin_addr);

    std::thread(handle_server_commands).detach();
    register_client();

    while (running) {
        sleep(3);
        if (sendto(client_socket, "PING", 4, 0,
                   (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("Ошибка отправки PING");
        }
    }

    close(client_socket);
    return 0;
}
