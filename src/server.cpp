// src/server.cpp

#include "common.h"      // Общие константы и print_error
#include <iostream>      // Ввод/вывод
#include <string>        // std::string
#include <vector>
#include <set>           // std::set для хранения клиентов
#include <thread>        // std::thread для фонового прослушивания
#include <mutex>         // std::mutex для защиты общих данных (сетов)
#include <atomic>        // std::atomic для флага остановки
#include <chrono>        // std::chrono для пауз и таймаутов
#include <sstream>       // std::stringstream для форматирования адреса
#include <iomanip>       // std::put_time для форматирования времени
#include <ctime>         // std::localtime
#include <cstring>       // memset, strerror

// --- Linux/POSIX Заголовки ---
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h> // Для inet_ntop/pton
#include <unistd.h>    // close()
#include <csignal>     // signal() для обработки Ctrl+C

// --- Определения типов и констант для Linux ---
using SocketType = int;
using SockLenType = socklen_t;
const SocketType SINVALID = -1;
const int SERROR = -1;
#define closesocket(s) close(s) // Макрос для совместимости

// --- Глобальные переменные состояния сервера ---
std::set<std::string> all_known_clients;          // Множество всех когда-либо виденных клиентов ("ip:port")
std::set<std::string> live_clients_current_cycle; // Множество клиентов, ответивших в текущем цикле
std::mutex client_mutex;                          // Мьютекс для защиты доступа к all_known_clients и live_clients_current_cycle
std::atomic<bool> stop_server(false);             // Атомарный флаг для сигнализации остановки сервера

// --- Вспомогательная функция: Преобразование sockaddr_in в строку "ip:port" ---
std::string get_ip_port_str(const sockaddr_in& addr) {
    char ip_str[INET_ADDRSTRLEN];
    // Преобразуем IP-адрес из бинарного формата в строку
    if (inet_ntop(AF_INET, &(addr.sin_addr), ip_str, INET_ADDRSTRLEN) == nullptr) {
        // В случае ошибки возвращаем пустую строку или другое обозначение
        return "invalid_ip";
    }
    // Используем stringstream для форматирования строки
    std::stringstream ss;
    ss << ip_str << ":" << ntohs(addr.sin_port); // ntohs преобразует порт из сетевого порядка в хостовый
    return ss.str();
}

// --- Обработчик сигнала SIGINT (Ctrl+C) ---
void handle_sigint_server(int sig) {
    std::cout << "\n[*] SIGINT received, initiating server shutdown..." << std::endl;
    stop_server = true; // Устанавливаем флаг для остановки основного цикла и потока
}

// --- Функция потока для прослушивания PONG-ответов от клиентов ---
void listen_for_responses(SocketType listen_sock) {
    std::cout << "[Listener Thread] Started. Waiting for PONG responses on port " << SERVER_LISTEN_PORT << "." << std::endl;

    char buffer[1024];           // Буфер для приема данных
    sockaddr_in client_addr{};   // Структура для хранения адреса клиента, приславшего PONG
    SockLenType client_addr_len = sizeof(client_addr);

    // Устанавливаем таймаут на получение (SO_RCVTIMEO) на слушающем сокете
    // Это нужно, чтобы поток не блокировался навсегда в recvfrom,
    // а периодически проверял флаг stop_server
    struct timeval tv;
    tv.tv_sec = 0;            // Секунды таймаута
    tv.tv_usec = 500000;      // Микросекунды таймаута (0.5 секунды)
    if (setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO, (const void*)&tv, sizeof(tv)) < 0) {
         print_error("Listener Thread: setsockopt SO_RCVTIMEO failed");
         // Поток может продолжать работать без таймаута, но может "зависнуть"
         // при остановке, если не приходят пакеты. Лучше завершить поток.
         std::cerr << "[Listener Thread] Error setting timeout. Thread will exit." << std::endl;
         return;
    }

    // Цикл прослушивания, пока не будет установлен флаг остановки
    while (!stop_server) {
        memset(&client_addr, 0, sizeof(client_addr)); // Очищаем структуру адреса перед вызовом
        client_addr_len = sizeof(client_addr);

        // Ожидаем получения данных (с таймаутом)
        int bytes_received = recvfrom(listen_sock, buffer, sizeof(buffer) - 1, 0,
                                      (sockaddr*)&client_addr, &client_addr_len);

        if (bytes_received == SERROR) {
            // Проверяем код ошибки (errno)
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Это ожидаемый таймаут, не ошибка. Просто продолжаем цикл.
                continue;
            } else if (errno == EINTR) {
                 // Прервано сигналом (вероятно, при остановке)
                 continue;
            } else {
                // Произошла другая ошибка сокета
                if (!stop_server) { // Не выводим ошибку, если нас штатно останавливают
                     print_error("Listener Thread: recvfrom failed");
                }
                // При серьезных ошибках можно было бы выйти из цикла: break;
                // Дадим небольшую паузу, чтобы не грузить CPU при постоянной ошибке
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        // Если данные успешно получены
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0'; // Добавляем null-терминатор
            std::string received_message(buffer);
            std::string client_id = get_ip_port_str(client_addr); // Получаем "ip:port" клиента

            // Проверяем, является ли сообщение ожидаемым PONG'ом
            if (received_message == CLIENT_PONG_MESSAGE) {
                // Блокируем мьютекс для безопасного доступа к общим сетам
                std::lock_guard<std::mutex> guard(client_mutex);

                // Выводим информацию о полученном PONG (можно вынести за мьютекс, если нужно)
                std::cout << "[Listener Thread] Received PONG from: " << client_id << std::endl;

                // Добавляем клиента в список живых для текущего цикла
                live_clients_current_cycle.insert(client_id);
                // Добавляем клиента в общий список известных (set сам обработает дубликаты)
                all_known_clients.insert(client_id);
            } else {
                 // Получено неожиданное сообщение
                 std::cout << "[Listener Thread] Received unexpected message from " << client_id
                           << ": \"" << received_message << "\"" << std::endl;
            }
        }
    } // Конец цикла while (!stop_server)

    std::cout << "[Listener Thread] Stopped." << std::endl;
}

// --- Основная функция сервера ---
int main() {
    // Инициализация сети (на Linux не требуется, но вызываем для порядка)
    initialize_networking();

    // Устанавливаем обработчик сигнала Ctrl+C (SIGINT)
    signal(SIGINT, handle_sigint_server);

    // === 1. Создание сокета для ОТПРАВКИ Multicast PING ===
    SocketType send_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (send_sock == SINVALID) {
        print_error("Main: Failed to create send socket");
        return 1;
    }
    std::cout << "[Main] Multicast send socket created." << std::endl;

    // Установка TTL (Time-To-Live) для multicast пакетов
    int mc_ttl = MULTICAST_TTL;
    if (setsockopt(send_sock, IPPROTO_IP, IP_MULTICAST_TTL, (const void*)&mc_ttl, sizeof(mc_ttl)) == SERROR) {
        print_error("Main: Failed to set multicast TTL on send socket");
        closesocket(send_sock);
        return 1;
    }

    // Настройка адреса multicast группы для отправки
    sockaddr_in mc_addr{};
    memset(&mc_addr, 0, sizeof(mc_addr));
    mc_addr.sin_family = AF_INET;
    mc_addr.sin_port = htons(MULTICAST_PORT); // Порт multicast группы
    if (inet_pton(AF_INET, MULTICAST_GROUP.c_str(), &mc_addr.sin_addr) <= 0) { // Адрес multicast группы
        print_error("Main: Invalid multicast group address format " + MULTICAST_GROUP);
        closesocket(send_sock);
        return 1;
    }


    // === 2. Создание сокета для ПРИЕМА Unicast PONG ===
    SocketType listen_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (listen_sock == SINVALID) {
        print_error("Main: Failed to create listen socket");
        closesocket(send_sock); // Закрываем и первый сокет
        return 1;
    }
     std::cout << "[Main] Unicast listen socket created." << std::endl;

    // Разрешаем переиспользование адреса (SO_REUSEADDR) для слушающего сокета
    int reuse = 1;
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const void*)&reuse, sizeof(reuse)) == SERROR) {
        print_error("Main: Failed to set SO_REUSEADDR on listen socket (non-critical)");
        // Не фатально, но может помешать быстрому перезапуску сервера
    }

    // Привязка (bind) слушающего сокета к нужному порту и всем интерфейсам
    sockaddr_in server_listen_addr{};
    memset(&server_listen_addr, 0, sizeof(server_listen_addr));
    server_listen_addr.sin_family = AF_INET;
    server_listen_addr.sin_port = htons(SERVER_LISTEN_PORT); // <<<--- Порт 5008 для PONG
    server_listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);  // Слушать на всех интерфейсах

    if (bind(listen_sock, (sockaddr*)&server_listen_addr, sizeof(server_listen_addr)) == SERROR) {
        print_error("Main: Failed to bind listen socket to port " + std::to_string(SERVER_LISTEN_PORT));
        closesocket(send_sock);
        closesocket(listen_sock);
        return 1;
    }
     std::cout << "[Main] Listen socket bound successfully to port " << SERVER_LISTEN_PORT << "." << std::endl;


    // === 3. Запуск потока для прослушивания PONG ответов ===
    std::thread listener_thread(listen_for_responses, listen_sock);
    std::cout << "[Main] Listener thread started." << std::endl;


    // === 4. Основной цикл работы сервера ===
    std::cout << "[Main] Server main loop starting. Sending PING to " << MULTICAST_GROUP << ":" << MULTICAST_PORT << "." << std::endl;
    std::cout << "[Main] Press Ctrl+C to stop." << std::endl;

    try {
        while (!stop_server) { // Работаем, пока не установлен флаг остановки
            auto now = std::chrono::system_clock::now();
            auto now_c = std::chrono::system_clock::to_time_t(now);
            std::tm now_tm = *std::localtime(&now_c); // Потокобезопасный localtime

            std::cout << "\n--- Cycle Start (" << std::put_time(&now_tm, "%H:%M:%S") << ") ---" << std::endl;

            // 4.1. Очищаем список живых для текущего цикла (под мьютексом)
            { // Область видимости для lock_guard
                std::lock_guard<std::mutex> guard(client_mutex);
                live_clients_current_cycle.clear();
            }

            // 4.2. Отправляем Multicast PING
            std::cout << "[Main] Sending PING to " << MULTICAST_GROUP << ":" << MULTICAST_PORT << "..." << std::endl;
            if (sendto(send_sock, SERVER_PING_MESSAGE.c_str(), SERVER_PING_MESSAGE.length(), 0,
                       (sockaddr*)&mc_addr, sizeof(mc_addr)) == SERROR) {
                // Обработка ошибок отправки (можно проверять errno)
                if (errno == ENETUNREACH || errno == EHOSTUNREACH || errno == ENONET) {
                     print_error("Main: Network/Host unreachable during PING send");
                     // Возможно, стоит подождать и попробовать снова
                     std::this_thread::sleep_for(std::chrono::seconds(1));
                } else {
                    print_error("Main: sendto PING failed");
                }
                // Можно продолжить цикл или выйти, если ошибка критична
            } else {
                 std::cout << "[Main] PING sent successfully." << std::endl;
            }

            // 4.3. Ждем время для сбора ответов (RESPONSE_TIMEOUT)
            std::cout << "[Main] Waiting " << RESPONSE_TIMEOUT.count() << " seconds for PONG responses..." << std::endl;
            // Используем цикл со sleep, чтобы можно было прерваться по флагу stop_server
            auto wakeUpTime = std::chrono::steady_clock::now() + RESPONSE_TIMEOUT;
            while (std::chrono::steady_clock::now() < wakeUpTime && !stop_server) {
                 std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Короткий сон для проверки флага
            }
            if (stop_server) break; // Выходим из основного цикла, если получили сигнал во время ожидания

            // 4.4. Анализируем результаты (под мьютексом)
            { // Область видимости для lock_guard
                std::lock_guard<std::mutex> guard(client_mutex);
                std::cout << "[Main] --- Cycle Analysis ---" << std::endl;
                std::cout << "  Total known clients: " << all_known_clients.size() << std::endl;
                std::cout << "  Live clients this cycle (" << live_clients_current_cycle.size() << "):" << std::endl;
                if (!live_clients_current_cycle.empty()) {
                    for (const auto& client_id : live_clients_current_cycle) {
                         std::cout << "    - " << client_id << std::endl;
                    }
                } else {
                     std::cout << "    <None>" << std::endl;
                }

                // Определяем пропавших клиентов (те, кто есть в all_known, но нет в live_current)
                std::set<std::string> missing_clients;
                 for (const auto& known : all_known_clients) {
                     // Если известного клиента нет в списке живых этого цикла
                     if (live_clients_current_cycle.find(known) == live_clients_current_cycle.end()) {
                         missing_clients.insert(known);
                     }
                 }

                std::cout << "  Missing clients this cycle (" << missing_clients.size() << "):" << std::endl;
                 if (!missing_clients.empty()) {
                     for (const auto& client_id : missing_clients) {
                         std::cout << "    - " << client_id << std::endl;
                     }
                     // Можно добавить логику удаления "старых" пропавших клиентов из all_known_clients
                     // например, если клиент отсутствует N циклов подряд.
                 } else {
                      std::cout << "    <None>" << std::endl;
                 }
                 std::cout << "[Main] --- End Cycle Analysis ---" << std::endl;

                // TODO: Здесь можно добавить логику для запроса системной информации
                // у клиентов из `live_clients_current_cycle` (отправка Unicast запроса)

            } // Мьютекс освобождается

            if (stop_server) break; // Проверяем флаг еще раз перед паузой

            // 4.5. Пауза перед следующим циклом (PING_INTERVAL)
            std::cout << "[Main] Next PING cycle in " << PING_INTERVAL.count() << " seconds..." << std::endl;
            wakeUpTime = std::chrono::steady_clock::now() + PING_INTERVAL;
            while (std::chrono::steady_clock::now() < wakeUpTime && !stop_server) {
                 std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
             if (stop_server) break; // Выходим, если сигнал пришел во время паузы

        } // Конец основного цикла while (!stop_server)

    } catch (const std::exception& e) {
        std::cerr << "[Main] Exception caught in main loop: " << e.what() << std::endl;
        stop_server = true; // Инициируем остановку при исключении
    } catch (...) {
        std::cerr << "[Main] Unknown exception caught in main loop." << std::endl;
        stop_server = true; // Инициируем остановку
    }


    // === 5. Завершение работы ===
    std::cout << "[Main] Main loop finished. Waiting for listener thread to stop..." << std::endl;

    // Флаг stop_server уже должен быть true
    // Ждем завершения потока слушателя
    if (listener_thread.joinable()) {
        listener_thread.join();
    } else {
         std::cerr << "[Main] Warning: Listener thread is not joinable!" << std::endl;
    }
    std::cout << "[Main] Listener thread joined." << std::endl;

    std::cout << "[Main] Closing sockets..." << std::endl;
    closesocket(send_sock);    // Закрываем сокет отправки multicast
    closesocket(listen_sock);  // Закрываем сокет приема unicast
    std::cout << "[Main] Sockets closed." << std::endl;

    // Очистка сети (на Linux не требуется)
    cleanup_networking();

    std::cout << "[Main] Server stopped." << std::endl;
    return 0; // Успешное завершение
}