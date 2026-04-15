#include <iostream>
#include <cstring>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>

#define MAX_NAME 32
#define MAX_PAYLOAD 256
#define MAX_TIME_STR 32
#define PORT 8080
#define RECONNECT_DELAY 2

typedef struct {
    uint32_t length;
    uint8_t  type;
    uint32_t msg_id;
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    time_t timestamp;
    char payload[MAX_PAYLOAD];
} MessageEx;

enum {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6,
    MSG_AUTH = 7,
    MSG_PRIVATE = 8,
    MSG_ERROR = 9,
    MSG_SERVER_INFO = 10,
    MSG_LIST = 11,
    MSG_HISTORY = 12,
    MSG_HISTORY_DATA = 13,
    MSG_HELP = 14
};

static int sockfd = -1;
static bool running = true;
static char nick[MAX_NAME] = "";
static pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;

void format_time(time_t t, char* buf, size_t sz)
{
    struct tm* tm_info = localtime(&t);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", tm_info);
}

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

static uint32_t local_id = 0;
bool send_msgex(int fd, uint8_t type, const char* text, const char* receiver = "")
{
    MessageEx msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.msg_id = htonl(++local_id);
    msg.timestamp = time(NULL);
    strncpy(msg.sender, nick, MAX_NAME - 1);
    if (receiver) strncpy(msg.receiver, receiver, MAX_NAME - 1);
    size_t plen = text ? strlen(text) : 0;
    if (plen >= MAX_PAYLOAD) plen = MAX_PAYLOAD - 1;
    if (text) memcpy(msg.payload, text, plen);
    msg.payload[plen] = '\0';
    uint32_t wire_len = sizeof(uint8_t) + sizeof(uint32_t) + MAX_NAME + MAX_NAME + sizeof(time_t) + (uint32_t)(plen + 1);
    msg.length = htonl(wire_len);

    if (!send_all(fd, &msg.length, sizeof(uint32_t))) return false;
    if (!send_all(fd, &msg.type, sizeof(uint8_t))) return false;
    if (!send_all(fd, &msg.msg_id, sizeof(uint32_t))) return false;
    if (!send_all(fd, msg.sender, MAX_NAME)) return false;
    if (!send_all(fd, msg.receiver, MAX_NAME)) return false;
    if (!send_all(fd, &msg.timestamp, sizeof(time_t))) return false;
    if (!send_all(fd, msg.payload, plen + 1)) return false;
    return true;
}

bool recv_msgex(int fd, MessageEx& msg)
{
    if (!recv_all(fd, &msg.length, sizeof(uint32_t))) return false;
    uint32_t wire_len = ntohl(msg.length);
    if (wire_len == 0 || wire_len > sizeof(MessageEx)) return false;
    if (!recv_all(fd, &msg.type, sizeof(uint8_t))) return false;
    if (!recv_all(fd, &msg.msg_id, sizeof(uint32_t))) return false;
    msg.msg_id = ntohl(msg.msg_id);
    if (!recv_all(fd, msg.sender, MAX_NAME)) return false;
    if (!recv_all(fd, msg.receiver, MAX_NAME)) return false;
    if (!recv_all(fd, &msg.timestamp, sizeof(time_t))) return false;
    uint32_t plen = wire_len - sizeof(uint8_t) - sizeof(uint32_t) - MAX_NAME - MAX_NAME - sizeof(time_t);
    if (plen > MAX_PAYLOAD) return false;
    if (!recv_all(fd, msg.payload, plen)) return false;
    msg.payload[plen > 0 ? plen - 1 : 0] = '\0';
    return true;
}

bool do_handshake(int fd)
{
    if (!send_msgex(fd, MSG_HELLO, nick)) return false;
    MessageEx msg;
    if (!recv_msgex(fd, msg) || msg.type != MSG_WELCOME) return false;
    if (!send_msgex(fd, MSG_AUTH, nick)) return false;
    if (!recv_msgex(fd, msg)) return false;
    if (msg.type == MSG_ERROR)
    {
        std::cout << "[AUTH ERROR]: " << msg.payload << std::endl;
        return false;
    }
    if (msg.type == MSG_SERVER_INFO) std::cout << "[SERVER]: " << msg.payload << std::endl;
    return true;
}

void* recv_thread(void*)
{
    MessageEx msg;
    while (running)
    {
        pthread_mutex_lock(&sock_mutex);
        int fd = sockfd;
        pthread_mutex_unlock(&sock_mutex);
        if (fd < 0) { sleep(1); continue; }

        if (!recv_msgex(fd, msg))
        {
            std::cout << "\nDisconnected. Reconnecting in " << RECONNECT_DELAY << "s..." << std::endl;
            pthread_mutex_lock(&sock_mutex);
            close(sockfd); sockfd = -1;
            pthread_mutex_unlock(&sock_mutex);
            sleep(RECONNECT_DELAY);
            while (running)
            {
                int new_fd = socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in sa;
                memset(&sa, 0, sizeof(sa));
                sa.sin_family = AF_INET;
                sa.sin_port = htons(PORT);
                inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
                if (connect(new_fd, (sockaddr*)&sa, sizeof(sa)) == 0) {
                    if (do_handshake(new_fd)) {
                        std::cout << "Reconnected." << std::endl;
                        std::cout << "> " << std::flush;
                        pthread_mutex_lock(&sock_mutex);
                        sockfd = new_fd;
                        pthread_mutex_unlock(&sock_mutex);
                        break;
                    }
                    close(new_fd);
                    running = false;
                    return NULL;
                }
                close(new_fd);
                std::cout << "Retry in " << RECONNECT_DELAY << "s..." << std::endl;
                sleep(RECONNECT_DELAY);
            }
            continue;
        }

        char time_buf[MAX_TIME_STR];
        format_time(msg.timestamp ? msg.timestamp : time(NULL), time_buf, sizeof(time_buf));
        switch (msg.type)
        {
            case MSG_TEXT:
                std::cout << "\n[" << time_buf << "][id=" << msg.msg_id << "][" << msg.sender << "]: " << msg.payload << std::endl;
                std::cout << "[CLIENT]: hmm... TCP feels stable today" << std::endl;
                std::cout << "> " << std::flush;
                break;
            case MSG_PRIVATE:
                std::cout << "\n" << msg.payload << std::endl;
                if (strstr(msg.payload, "[OFFLINE]")) std::cout << "[SERVER]: message delivered (maybe)" << std::endl;
                std::cout << "> " << std::flush;
                break;
            case MSG_SERVER_INFO:
                std::cout << "\n[SERVER]: " << msg.payload << std::endl;
                std::cout << "> " << std::flush;
                break;
            case MSG_HISTORY_DATA:
                std::cout << "\n" << msg.payload << std::flush;
                std::cout << "[LOG]: i love TCP/IP (don't tell UDP)" << std::endl;
                std::cout << "> " << std::flush;
                break;
            case MSG_ERROR:
                std::cout << "\n[ERROR]: " << msg.payload << std::endl;
                std::cout << "> " << std::flush;
                break;
            case MSG_PONG:
                std::cout << "\n[SERVER]: PONG" << std::endl;
                std::cout << "[LOG]: i love cast (no segmentation faults pls)" << std::endl;
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

int main()
{
    std::cout << "Enter nickname: ";
    std::string n;
    std::getline(std::cin, n);
    strncpy(nick, n.c_str(), MAX_NAME - 1);
    nick[MAX_NAME - 1] = '\0';
    if (strlen(nick) == 0) { std::cerr << "Nickname can't be empty" << std::endl; return 1; }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }
    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(sockfd, (sockaddr*)&sa, sizeof(sa)) < 0) { perror("connect"); return 1; }
    std::cout << "Connected" << std::endl;

    if (!do_handshake(sockfd)) { close(sockfd); return 1; }
    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread, NULL);
    pthread_detach(tid);
    while (running)
    {
        std::cout << "> " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            pthread_mutex_lock(&sock_mutex);
            if (sockfd >= 0) send_msgex(sockfd, MSG_BYE, "");
            pthread_mutex_unlock(&sock_mutex);
            break;
        }

        pthread_mutex_lock(&sock_mutex);
        int fd = sockfd;
        pthread_mutex_unlock(&sock_mutex);
        if (fd < 0) { std::cout << "(not connected)" << std::endl; continue; }
        if (line == "/quit")
        {
            send_msgex(fd, MSG_BYE, "");
            std::cout << "Disconnected" << std::endl;
            running = false;
        }
        else if (line == "/ping")
        {
            send_msgex(fd, MSG_PING, "");
        }
        else if (line == "/list")
        {
            send_msgex(fd, MSG_LIST, "");
        }
        else if (line == "/history")
        {
            send_msgex(fd, MSG_HISTORY, "");
        }
        else if (line.substr(0, 9) == "/history ")
        {
            std::string nstr = line.substr(9);
            int n = atoi(nstr.c_str());
            if (n <= 0) { std::cout << "Usage: /history N (N > 0)" << std::endl; continue; }
            send_msgex(fd, MSG_HISTORY, nstr.c_str());
        }
        else if (line == "/help")
        {
            std::cout << "Available commands:" << std::endl;
            std::cout << "/help" << std::endl;
            std::cout << "/history" << std::endl;
            std::cout << "/history N" << std::endl;
            std::cout << "/list" << std::endl;
            std::cout << "/quit" << std::endl;
            std::cout << "/w <nick> <message>" << std::endl;
            std::cout << "/ping" << std::endl;
            std::cout << "Tip: meow, message delivered" << std::endl;
        }
        else if (line.substr(0, 3) == "/w ")
        {
            std::string rest = line.substr(3);
            size_t sp = rest.find(' ');
            if (sp == std::string::npos)
            {
                std::cout << "Usage: /w <nick> <message>" << std::endl;
            }
            else
            {
                std::string target = rest.substr(0, sp);
                std::string text = rest.substr(sp + 1);
                std::string payload = target + ":" + text;
                send_msgex(fd, MSG_PRIVATE, payload.c_str(), target.c_str());
            }
        }
        else if (!line.empty())
        {
            send_msgex(fd, MSG_TEXT, line.c_str());
        }
    }

    pthread_mutex_lock(&sock_mutex);
    if (sockfd >= 0) { close(sockfd); sockfd = -1; }
    pthread_mutex_unlock(&sock_mutex);
    return 0;
}
