#include <iostream>
#include <cstring>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdint.h>
#include <pthread.h>

#define MAX_PAYLOAD 1024
#define PORT 8080
#define RECONNECT_DELAY 2

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

// сетевые утилиты
bool send_all(int fd, const void* buf, size_t len)
{
    const char* p = (const char*)buf;
    while (len > 0)
    {
        ssize_t s = send(fd, p, len, 0);
        if (s <= 0) return false;
        p += s; len -= s;
    }
    return true;
}

bool recv_all(int fd, void* buf, size_t len)
{
    char* p = (char*)buf;
    while (len > 0)
    {
        ssize_t r = recv(fd, p, len, 0);
        if (r <= 0) return false;
        p += r; len -= r;
    }
    return true;
}

bool send_msg(int fd, uint8_t type, const char* text)
{
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    size_t plen = text ? strlen(text) : 0;
    if (plen > MAX_PAYLOAD) plen = MAX_PAYLOAD;
    if (text) memcpy(msg.payload, text, plen);
    msg.length = htonl((uint32_t)(1 + plen));
    return send_all(fd, &msg, sizeof(uint32_t) + 1 + plen);
}

bool recv_msg(int fd, Message& msg)
{
    if (!recv_all(fd, &msg.length, sizeof(uint32_t))) return false;
    uint32_t len = ntohl(msg.length);
    if (len == 0 || len > MAX_PAYLOAD + 1) return false;
    if (!recv_all(fd, &msg.type, len)) return false;
    msg.payload[len - 1] = '\0';
    return true;
}

// общее состояние
static int sockfd   = -1;
static bool running  = true;
static char nick[64] = "";
static pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;

// поток приема
void* recv_thread(void* arg)
{
    (void)arg;
    Message msg;
    while (running)
    {
        pthread_mutex_lock(&sock_mutex);
        int fd = sockfd;
        pthread_mutex_unlock(&sock_mutex);
        if (fd < 0) {sleep(1); continue;}
        if (!recv_msg(fd, msg))
        {
            std::cout << "\nDisconnected. Reconnecting in " << RECONNECT_DELAY << "s..." << std::endl;
            pthread_mutex_lock(&sock_mutex);
            close(sockfd);
            sockfd = -1;
            pthread_mutex_unlock(&sock_mutex);
            sleep(RECONNECT_DELAY);

            // переподключение
            while (running)
            {
                int new_fd = socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in sa;
                memset(&sa, 0, sizeof(sa));
                sa.sin_family = AF_INET;
                sa.sin_port   = htons(PORT);
                inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
                if (connect(new_fd, (sockaddr*)&sa, sizeof(sa)) == 0)
                {
                    // тут HELLO и WELCOME
                    send_msg(new_fd, MSG_HELLO, nick);
                    Message wm;
                    if (recv_msg(new_fd, wm) && wm.type == MSG_WELCOME)
                    {
                        std::cout << "Reconnected. Welcome " << wm.payload << std::endl;
                        std::cout << "> " << std::flush;
                        pthread_mutex_lock(&sock_mutex);
                        sockfd = new_fd;
                        pthread_mutex_unlock(&sock_mutex);
                        break;
                    }
                }
                close(new_fd);
                std::cout << "Retry in " << RECONNECT_DELAY << "s..." << std::endl;
                sleep(RECONNECT_DELAY);
            }
            continue;
        }

        switch (msg.type)
        {
            case MSG_TEXT:
                std::cout << "\n" << msg.payload << std::endl;
                std::cout << "> " << std::flush;
                break;
            case MSG_PONG:
                std::cout << "\nPONG" << std::endl;
                std::cout << "> " << std::flush;
                break;
            case MSG_BYE:
                std::cout << "\nDisconnected by server." << std::endl;
                running = false;
                break;
            default:
                break;
        }
    }
    return NULL;
}

int main() {
    std::cout << "Enter nickname: ";
    std::string n;
    std::getline(std::cin, n);
    strncpy(nick, n.c_str(), sizeof(nick) - 1);

    // Первое подключение
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(sockfd, (sockaddr*)&sa, sizeof(sa)) < 0) { perror("connect"); return 1; }
    std::cout << "Connected" << std::endl;

    send_msg(sockfd, MSG_HELLO, nick);
    Message msg;
    if (!recv_msg(sockfd, msg) || msg.type != MSG_WELCOME)
    {
        std::cerr << "Expected MSG_WELCOME" << std::endl;
        close(sockfd); return 1;
    }
    std::cout << "Welcome " << msg.payload << std::endl;

    // запускаем поток приема
    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread, NULL);
    pthread_detach(tid);

    // главный цикл ввода
    while (running)
    {
        std::cout << "> " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line))
        {
            pthread_mutex_lock(&sock_mutex);
            if (sockfd >= 0) send_msg(sockfd, MSG_BYE, "");
            pthread_mutex_unlock(&sock_mutex);
            break;
        }
        pthread_mutex_lock(&sock_mutex);
        int fd = sockfd;
        pthread_mutex_unlock(&sock_mutex);
        if (fd < 0) {std::cout << "(not connected)" << std::endl; continue;}
        if (line == "/quit")
        {
            send_msg(fd, MSG_BYE, "");
            std::cout << "Disconnected" << std::endl;
            running = false;
        }
        else if (line == "/ping")
        {
            send_msg(fd, MSG_PING, "");
        }
        else if (!line.empty())
        {
            send_msg(fd, MSG_TEXT, line.c_str());
        }
    }
    pthread_mutex_lock(&sock_mutex);
    if (sockfd >= 0) {close(sockfd); sockfd = -1;}
    pthread_mutex_unlock(&sock_mutex);
    return 0;
}
