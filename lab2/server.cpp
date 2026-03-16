#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdint.h>

#define MAX_PAYLOAD 1024
#define PORT 8080

typedef struct {
    uint32_t length;       // длина поля type + payload
    uint8_t  type;
    char     payload[MAX_PAYLOAD];
} Message;

enum {
    MSG_HELLO   = 1,
    MSG_WELCOME = 2,
    MSG_TEXT    = 3,
    MSG_PING    = 4,
    MSG_PONG    = 5,
    MSG_BYE     = 6
};

// Надёжная отправка всех байт
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

// Надёжное чтение нужного количества байт
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
    // length = sizeof(type) + payload length
    msg.length = htonl((uint32_t)(1 + plen));
    // отправляем: 4 байта length + 1 байт type + payload
    size_t total = sizeof(uint32_t) + 1 + plen;
    return send_all(fd, &msg, total);
}

bool recv_msg(int fd, Message& msg) {
    // читаем 4 байта length
    if (!recv_all(fd, &msg.length, sizeof(uint32_t))) return false;
    uint32_t len = ntohl(msg.length);
    if (len == 0 || len > MAX_PAYLOAD + 1) return false;
    // читаем type + payload (len байт)
    if (!recv_all(fd, &msg.type, len)) return false;
    msg.payload[len - 1] = '\0'; // null-terminate payload
    return true;
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, 1) < 0) { perror("listen"); return 1; }

    std::cout << "Server listening on port " << PORT << std::endl;

    sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) { perror("accept"); return 1; }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(client_addr.sin_port);
    std::cout << "Client connected" << std::endl;

    // Ожидаем MSG_HELLO
    Message msg;
    if (!recv_msg(client_fd, msg) || msg.type != MSG_HELLO) {
        std::cerr << "Expected MSG_HELLO" << std::endl;
        close(client_fd); close(server_fd); return 1;
    }
    std::cout << "[" << client_ip << ":" << client_port << "]: Hello " << msg.payload << std::endl;

    // Отправляем MSG_WELCOME
    char welcome[64];
    snprintf(welcome, sizeof(welcome), "%s:%d", client_ip, client_port);
    send_msg(client_fd, MSG_WELCOME, welcome);

    // Основной цикл
    while (true) {
        if (!recv_msg(client_fd, msg)) {
            std::cout << "Client disconnected" << std::endl;
            break;
        }
        switch (msg.type) {
            case MSG_TEXT:
                std::cout << "[" << client_ip << ":" << client_port << "]: " << msg.payload << std::endl;
                break;
            case MSG_PING:
                send_msg(client_fd, MSG_PONG, "");
                break;
            case MSG_BYE:
                std::cout << "Client disconnected" << std::endl;
                goto done;
            default:
                break;
        }
    }
done:
    close(client_fd);
    close(server_fd);
    return 0;
}
