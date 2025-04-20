#include "common.h"
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <atomic>
#include <csignal>
#include <chrono>
#include <thread>
#include <cstdlib>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using SocketType = int;
using SockLenType = socklen_t;
const SocketType SINVALID = -1;
const int SERROR = -1;
#define closesocket(s) close(s)

std::atomic<bool> stop_client(false);

void handle_sigint_client(int sig) {
    std::cout << "\n[*] SIGINT received, stopping client gracefully..." << std::endl;
    stop_client = true;
}

int main() {
    initialize_networking();
    signal(SIGINT, handle_sigint_client);

    const char* server_ip_env = std::getenv("SERVER_UNICAST_IP");
    std::string server_unicast_ip;
    if (server_ip_env == nullptr || std::strlen(server_ip_env) == 0) {
        std::cerr << "[!] Warning: SERVER_UNICAST_IP environment variable not set or empty. Using default: 127.0.0.1" << std::endl;
        server_unicast_ip = "127.0.0.1";
    } else {
        server_unicast_ip = server_ip_env;
    }
    std::cout << "[*] Target Server IP (from env or default): " << server_unicast_ip << std::endl;

    // --- СОКЕТ 1: Для приема Multicast PING ---
    SocketType recv_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (recv_sock == SINVALID) {
        print_error("Failed to create receive socket");
        return 1;
    }
    std::cout << "[*] Receive socket created successfully." << std::endl;

    int reuse = 1;
    if (setsockopt(recv_sock, SOL_SOCKET, SO_REUSEADDR, (const void*)&reuse, sizeof(reuse)) == SERROR) {
        print_error("Failed to set SO_REUSEADDR on receive socket (non-critical)");
    }

    sockaddr_in local_recv_addr{};
    memset(&local_recv_addr, 0, sizeof(local_recv_addr));
    local_recv_addr.sin_family = AF_INET;
    local_recv_addr.sin_port = htons(MULTICAST_PORT); // Порт 5007
    local_recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(recv_sock, (sockaddr*)&local_recv_addr, sizeof(local_recv_addr)) == SERROR) {
        print_error("Failed to bind receive socket to port " + std::to_string(MULTICAST_PORT));
        closesocket(recv_sock);
        return 1;
    }
    std::cout << "[*] Receive socket bound successfully to port " << MULTICAST_PORT << "." << std::endl;

    struct ip_mreq mreq{};
    if (inet_pton(AF_INET, MULTICAST_GROUP.c_str(), &mreq.imr_multiaddr.s_addr) <= 0) {
         print_error("Invalid multicast group address format " + MULTICAST_GROUP);
         closesocket(recv_sock);
         return 1;
    }
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(recv_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const void*)&mreq, sizeof(mreq)) == SERROR) {
        print_error("Failed to join multicast group " + MULTICAST_GROUP);
        closesocket(recv_sock);
        return 1;
    }
    std::cout << "[*] Successfully joined multicast group " << MULTICAST_GROUP << "." << std::endl;

    struct timeval recv_tv;
    recv_tv.tv_sec = 1;
    recv_tv.tv_usec = 0;
    if (setsockopt(recv_sock, SOL_SOCKET, SO_RCVTIMEO, (const void*)&recv_tv, sizeof(recv_tv)) < 0) {
        print_error("Failed to set receive timeout (SO_RCVTIMEO), Ctrl+C might be delayed");
    }
    // --- Конец настройки сокета 1 ---

    // --- СОКЕТ 2: Для отправки Unicast PONG ---
    SocketType send_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
     if (send_sock == SINVALID) {
        print_error("Failed to create send socket");
        closesocket(recv_sock); // Закрываем и первый сокет
        return 1;
    }
    std::cout << "[*] Send socket created successfully (will use ephemeral port)." << std::endl;
    // НЕ ДЕЛАЕМ BIND для send_sock!
    // --- Конец настройки сокета 2 ---


    // --- Настройка адреса сервера для отправки PONG ---
    sockaddr_in server_pong_addr{};
    memset(&server_pong_addr, 0, sizeof(server_pong_addr));
    server_pong_addr.sin_family = AF_INET;
    server_pong_addr.sin_port = htons(SERVER_LISTEN_PORT); // Порт 5008
    if (inet_pton(AF_INET, server_unicast_ip.c_str(), &server_pong_addr.sin_addr) <= 0) {
         print_error("Invalid server IP address format " + server_unicast_ip);
         closesocket(recv_sock);
         closesocket(send_sock);
         return 1;
    }
     std::cout << "[*] Server address for PONG configured: " << server_unicast_ip << ":" << SERVER_LISTEN_PORT << std::endl;


    // --- Основной цикл клиента ---
    std::cout << "[*] Client is running. Waiting for PING messages on port " << MULTICAST_PORT << "..." << std::endl;
    std::cout << "[*] Press Ctrl+C to stop." << std::endl;

    char buffer[1024];
    sockaddr_in sender_addr{};
    SockLenType sender_addr_len = sizeof(sender_addr);

    while (!stop_client) {
        memset(&sender_addr, 0, sizeof(sender_addr));
        sender_addr_len = sizeof(sender_addr);

        // Слушаем PING на сокете recv_sock
        int bytes_received = recvfrom(recv_sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&sender_addr, &sender_addr_len);

        if (bytes_received == SERROR) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            } else if (errno == EINTR) {
                 continue;
            } else {
                if (!stop_client) {
                    print_error("recvfrom failed on receive socket");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            std::string received_message(buffer);

            char sender_ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(sender_addr.sin_addr), sender_ip_str, INET_ADDRSTRLEN);
            // Можно добавить вывод порта отправителя PING'а: ntohs(sender_addr.sin_port)
            std::cout << "\n[*] Received " << bytes_received << " bytes from "
                      << sender_ip_str //<< ":" << ntohs(sender_addr.sin_port)
                      << ": \"" << received_message << "\"" << std::endl;

            if (received_message == SERVER_PING_MESSAGE) {
                std::cout << "[*] PING received! Sending PONG to server ("
                          << server_unicast_ip << ":" << SERVER_LISTEN_PORT << ")..." << std::endl;

                // Отправляем PONG на сервер через send_sock
                if (sendto(send_sock, CLIENT_PONG_MESSAGE.c_str(), CLIENT_PONG_MESSAGE.length(), 0,
                           (sockaddr*)&server_pong_addr, sizeof(server_pong_addr)) == SERROR) {
                    print_error("sendto PONG failed on send socket");
                } else {
                    std::cout << "[*] PONG sent successfully." << std::endl;
                }
            } else {
                std::cout << "[?] Received unexpected message, ignoring." << std::endl;
            }
        }
    }

    // --- Завершение работы ---
    std::cout << "[*] Cleaning up and exiting..." << std::endl;

    // Покидаем Multicast группу (используя recv_sock, к которому она привязана)
    if (setsockopt(recv_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const void*)&mreq, sizeof(mreq)) == SERROR) {
        print_error("Failed to leave multicast group (non-critical)");
    } else {
        std::cout << "[*] Left multicast group " << MULTICAST_GROUP << "." << std::endl;
    }

    // Закрываем оба сокета
    closesocket(recv_sock);
    closesocket(send_sock);
    std::cout << "[*] Sockets closed." << std::endl;

    cleanup_networking();
    std::cout << "[*] Client stopped." << std::endl;
    return 0;
}