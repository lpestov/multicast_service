#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <chrono>


const std::string MULTICAST_GROUP = "224.1.1.1";
const unsigned short MULTICAST_PORT = 5007;
const unsigned short SERVER_LISTEN_PORT = 5008;
const std::string SERVER_PING_MESSAGE = "SERVER_DISCOVERY_PING";
const std::string CLIENT_PONG_MESSAGE = "CLIENT_DISCOVERY_PONG";
const int MULTICAST_TTL = 1;
const std::chrono::seconds RESPONSE_TIMEOUT(5);
const std::chrono::seconds PING_INTERVAL(10);

void print_error(const std::string& msg);

inline bool initialize_networking() { return true; }
inline void cleanup_networking() {}

#endif