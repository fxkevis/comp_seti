#include <iostream>
#include <cstring>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/select.h>

#define MAX_PAYLOAD 1024
#define PORT 8080

typedef struct {
    uint32_t length;
    uint8_t type;
    char payload[MAX_PAYLOAD];
} Message;

enum {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6
};

bool send_all(int fd, const void* buf, size_t len) {
    const char* ptr = (const char*)buf;
    while (len > 0) {
        ssize_t sent = send(fd, ptr, len, 0);
        if (sent <= 0) return false;
        ptr += sent;
        len -= sent;
    }
    return true;
}

bool recv_all(int fd, void* buf, size_t len) {
    char* ptr = (char*)buf;
    while (len > 0) {
        ssize_t r = recv(fd, ptr, len, 0);
        if (r <= 0) return false;
        ptr += r;
        len -= r;
    }
    return true;
}

bool send_msg(int fd, uint8_t type, const char* text) {
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    size_t plen = text ? strlen(text) : 0;
    if (plen > MAX_PAYLOAD) plen = MAX_PAYLOAD;
    if (text) memcpy(msg.payload, text, plen);
    msg.length = htonl((uint32_t)(1 + plen));
    size_t total = sizeof(uint32_t) + 1 + plen;
    return send_all(fd, &msg, total);
}

bool recv_msg(int fd, Message& msg) {
    if (!recv_all(fd, &msg.length, sizeof(uint32_t))) return false;
    uint32_t len = ntohl(msg.length);
    if (len == 0 || len > MAX_PAYLOAD + 1) return false;
    if (!recv_all(fd, &msg.type, len)) return false;
    msg.payload[len - 1] = '\0';
    return true;
}

int main() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(sockfd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect"); return 1;
    }
    std::cout << "Connected" << std::endl;

    // Отправляем MSG_HELLO с ником
    std::string nick;
    std::cout << "Enter nickname: ";
    std::getline(std::cin, nick);
    send_msg(sockfd, MSG_HELLO, nick.c_str());

    // Получаем MSG_WELCOME
    Message msg;
    if (!recv_msg(sockfd, msg) || msg.type != MSG_WELCOME) {
        std::cerr << "Expected MSG_WELCOME" << std::endl;
        close(sockfd); return 1;
    }
    std::cout << "Welcome " << msg.payload << std::endl;

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sockfd, &fds);
        FD_SET(STDIN_FILENO, &fds);

        int maxfd = sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO;
        if (select(maxfd + 1, &fds, nullptr, nullptr, nullptr) < 0) break;

        if (FD_ISSET(sockfd, &fds)) {
            if (!recv_msg(sockfd, msg)) {
                std::cout << "Disconnected" << std::endl;
                break;
            }
            switch (msg.type) {
                case MSG_TEXT:
                    std::cout << msg.payload << std::endl;
                    break;
                case MSG_PONG:
                    std::cout << "PONG" << std::endl;
                    break;
                case MSG_BYE:
                    std::cout << "Disconnected" << std::endl;
                    goto done;
                default:
                    break;
            }
            std::cout << "> " << std::flush;
        }

        if (FD_ISSET(STDIN_FILENO, &fds)) {
            std::string line;
            std::cout << "> ";
            if (!std::getline(std::cin, line)) {
                send_msg(sockfd, MSG_BYE, "");
                break;
            }
            if (line == "/quit") {
                send_msg(sockfd, MSG_BYE, "");
                std::cout << "Disconnected" << std::endl;
                break;
            } else if (line == "/ping") {
                send_msg(sockfd, MSG_PING, "");
            } else if (!line.empty()) {
                send_msg(sockfd, MSG_TEXT, line.c_str());
            }
        }
    }
done:
    close(sockfd);
    return 0;
}
