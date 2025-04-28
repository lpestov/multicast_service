#include <iostream>
#include <thread>
#include <csignal>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <random>
#include <vector>
#include <cstring>
#include <ctime>
#include <netdb.h>

bool running = true;
int client_socket;
sockaddr_in server_addr{};
std::string client_name;

void signal_handler(int sig) {
    std::cout << "[" << client_name << "] Получен сигнал " << sig << ", завершение..." << std::endl;
    running = false;
    shutdown(client_socket, SHUT_RDWR);
}

std::string get_hardware() {
    struct sysinfo info{};
    if (sysinfo(&info) != 0) {
        perror(("[" + client_name + "] sysinfo").c_str());
        return "CPU:N/A RAM:N/A";
    }
    long num_procs = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_procs < 0) {
        perror(("[" + client_name + "] sysconf").c_str());
        num_procs = 0;
    }
    unsigned long ram_mb = info.totalram / (1024 * 1024);
    return "CPU:" + std::to_string(num_procs) +
           " RAM:" + std::to_string(ram_mb) + "MB";
}

bool resolve_server_address(const char *hostname, int port, sockaddr_in &server_addr_out) {
    struct addrinfo hints, *res, *p;
    int status;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if ((status = getaddrinfo(hostname, std::to_string(port).c_str(), &hints, &res)) != 0) {
        std::cerr << "[" << client_name << "] Ошибка getaddrinfo для '" << hostname << "': " << gai_strerror(status) <<
                std::endl;
        return false;
    }

    bool found = false;
    for (p = res; p != NULL; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            memcpy(&server_addr_out, p->ai_addr, sizeof(sockaddr_in));
            char ipstr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((struct sockaddr_in *) p->ai_addr)->sin_addr, ipstr, sizeof ipstr);
            std::cout << "[" << client_name << "] Адрес сервера '" << hostname << "' разрешен в IP: " << ipstr <<
                    std::endl;
            found = true;
            break;
        }
    }

    freeaddrinfo(res);

    if (!found) {
        std::cerr << "[" << client_name << "] Не удалось найти IPv4 адрес для '" << hostname << "'" << std::endl;
    }

    return found;
}


void register_client() {
    std::string hardware_info = get_hardware();
    std::string msg = "REGISTER:" + client_name + ":" + hardware_info;
    std::cout << "[" << client_name << "] Попытка регистрации с данными: " << hardware_info << std::endl;
    ssize_t bytes_sent = sendto(client_socket, msg.c_str(), msg.size(), 0,
                                (sockaddr *) &server_addr, sizeof(server_addr));
    if (bytes_sent < 0) {
        perror(("[" + client_name + "] Ошибка отправки REGISTER").c_str());
    } else {
        std::cout << "[" << client_name << "] Отправлено REGISTER (" << bytes_sent << " байт)" << std::endl;
    }
}

void handle_server_commands() {
    // задержка ответа
    // std::uniform_int_distribution<> delay_distrib(10, 20);

    char buffer[1024];
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, 2);
    const std::vector<std::string> options = {"ROCK", "PAPER", "SCISSORS"};

    std::cout << "[" << client_name << "] Поток прослушивания сервера запущен." << std::endl;

    while (running) {
        sockaddr_in server_addr_tmp{};
        socklen_t addr_len = sizeof(server_addr_tmp);
        memset(buffer, 0, sizeof(buffer));

        ssize_t len = recvfrom(client_socket, buffer, sizeof(buffer) - 1, 0,
                               (sockaddr *) &server_addr_tmp, &addr_len);

        if (!running) break;

        if (len > 0) {
            std::string cmd(buffer, len);
            char sender_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &server_addr_tmp.sin_addr, sender_ip, sizeof(sender_ip));

            if (cmd == "CHOOSE") {

                // задержка ответа
                // int delay_s = delay_distrib(gen);
                // std::cout << "[" << client_name << "] Получена команда CHOOSE, задержка " << delay_s << " секунд." << std::endl;
                // std::this_thread::sleep_for(std::chrono::seconds(delay_s));

                std::string choice = options[distrib(gen)];
                std::cout << "[" << client_name << "] Получена команда CHOOSE, отправляем: " << choice << std::endl;
                ssize_t bytes_sent = sendto(client_socket, choice.c_str(), choice.size(), 0,
                                            (sockaddr *) &server_addr, sizeof(server_addr));
                if (bytes_sent < 0) {
                    perror(("[" + client_name + "] Ошибка отправки выбора").c_str());
                }
            } else if (cmd == "SHUTDOWN") {
                std::cout << "[" << client_name << "] Получена команда на отключение SHUTDOWN" << std::endl;
                running = false;
                break;
            } else {
                std::cout << "[" << client_name << "] Сообщение от сервера: " << cmd << std::endl;
            }
        } else if (len < 0) {
            if (running) {
                perror(("[" + client_name + "] Ошибка приема recvfrom").c_str());
            }
        }
    }
    std::cout << "[" << client_name << "] Поток прослушивания сервера завершен." << std::endl;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        client_name = "Client_" + std::to_string(getpid());
        std::cerr << "Предупреждение: Имя клиента не указано в аргументах, используется имя по умолчанию: " <<
                client_name << std::endl;
    } else {
        client_name = argv[1];
    }
    std::cout << "[" << client_name << "] Клиент запускается..." << std::endl;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    client_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_socket < 0) {
        perror(("[" + client_name + "] Ошибка создания сокета").c_str());
        return 1;
    }
    std::cout << "[" << client_name << "] Сокет создан." << std::endl;

    std::cout << "[" << client_name << "] Попытка разрешить имя хоста сервера 'server'..." << std::endl;
    if (!resolve_server_address("server", 8080, server_addr)) {
        std::cerr << "[" << client_name << "] Не удалось разрешить адрес сервера. Завершение." << std::endl;
        close(client_socket);
        return 1;
    }
    char server_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &server_addr.sin_addr, server_ip_str, sizeof(server_ip_str));
    std::cout << "[" << client_name << "] Адрес сервера " << server_ip_str << ":" << ntohs(server_addr.sin_port) <<
            " настроен." << std::endl;


    std::thread server_listener_thread(handle_server_commands);

    std::cout << "[" << client_name << "] Ожидание 2 секунды перед регистрацией..." << std::endl;
    sleep(2);

    register_client();

    std::cout << "[" << client_name << "] Вход в основной цикл отправки PING." << std::endl;
    time_t last_ping_log_time = 0;

    // для тестирования задержки пинга
    // std::random_device rd;
    // std::mt19937 gen(rd());
    // std::uniform_int_distribution<> ping_delay(5, 15);

    while (running) {
        sleep(3);

        // int delay = ping_delay(gen);
        // std::cout << "[" << client_name << "] Ожидание " << delay << " секунд перед отправкой PING..." << std::endl;
        // std::this_thread::sleep_for(std::chrono::seconds(delay));
        if (!running) break;

        ssize_t bytes_sent = sendto(client_socket, "PING", 4, 0,
                                    (sockaddr *) &server_addr, sizeof(server_addr));
        if (bytes_sent < 0) {
            if (running && errno != EBADF && errno != EPIPE) {
                perror(("[" + client_name + "] Ошибка отправки PING").c_str());
            }
        } else {
            time_t now = time(nullptr);
            if (now - last_ping_log_time >= 10) {
                std::cout << "[" << client_name << "] Отправлен PING (" << bytes_sent << " байт)" << std::endl;
                last_ping_log_time = now;
            }
        }
    }

    std::cout << "[" << client_name << "] Основной цикл завершен." << std::endl;

    if (server_listener_thread.joinable()) {
        std::cout << "[" << client_name << "] Ожидание завершения потока слушателя..." << std::endl;
        server_listener_thread.join();
    }

    close(client_socket);
    std::cout << "[" << client_name << "] Клиент завершил работу." << std::endl;
    return 0;
}
