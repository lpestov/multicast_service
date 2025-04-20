#include "common.h"
#include <iostream>
#include <errno.h>
#include <string.h>

void print_error(const std::string& msg) {
    std::cerr << "[!] Error: " << msg << " - " << strerror(errno)
              << " (errno: " << errno << ")" << std::endl;
}