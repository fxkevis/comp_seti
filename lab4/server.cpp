#include <iostream>
#include <cstring>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdint.h>
#include <pthread.h>
#include <queue>
#include <vector>

#define MAX_PAYLOAD 1024
#define PORT 8080
#define POOL_SIZE 10

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
    MSG_BYE = 6,
    MSG_AUTH = 7,
    MSG_PRIVATE = 8,
    MSG_ERROR = 9,
    MSG_SERVER_INFO = 10
};

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

    std::cout << "[Layer 7 - Application] prepare response (type=" << (int)type << ")" << std::endl;
    std::cout << "[Layer 6 - Presentation] serialize Message" << std::endl;
    std::cout << "[Layer 4 - Transport] send()" << std::endl;
    return send_all(fd, &msg, sizeof(uint32_t) + 1 + plen);
}

bool recv_msg(int fd, Message& msg)
{
    std::cout << "[Layer 4 - Transport] recv()" << std::endl;
    if (!recv_all(fd, &msg.length, sizeof(uint32_t))) return false;
    uint32_t len = ntohl(msg.length);
    if (len == 0 || len > MAX_PAYLOAD + 1) return false;
    if (!recv_all(fd, &msg.type, len)) return false;
    msg.payload[len - 1] = '\0';
    std::cout << "[Layer 6 - Presentation] deserialize Message (type=" << (int)msg.type << ")" << std::endl;
    return true;
}

typedef struct {
    int sock;
    char nickname[32];
    int authenticated;
} Client;

static std::vector<Client> clients;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

void broadcast(const char* text, int sender_fd)
{
    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < clients.size(); i++) 
    {
        if (clients[i].sock != sender_fd) send_msg(clients[i].sock, MSG_TEXT, text);
    }
    pthread_mutex_unlock(&clients_mutex);
}

bool nick_taken(const char* nick)
{
    for (size_t i = 0; i < clients.size(); i++)
        if (clients[i].authenticated && strcmp(clients[i].nickname, nick) == 0) return true;
    return false;
}

bool send_private(const char* target, const char* sender, const char* text)
{
    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < clients.size(); i++) 
    {
        if (clients[i].authenticated && strcmp(clients[i].nickname, target) == 0) 
        {
            char buf[MAX_PAYLOAD];
            snprintf(buf, sizeof(buf), "[PRIVATE][%s]: %s", sender, text);
            send_msg(clients[i].sock, MSG_PRIVATE, buf);
            pthread_mutex_unlock(&clients_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return false;
}

void remove_client(int fd)
{
    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < clients.size(); i++) 
    {
        if (clients[i].sock == fd) {
            clients.erase(clients.begin() + i);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

static std::queue<int> job_queue;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

void enqueue(int fd)
{
    pthread_mutex_lock(&queue_mutex);
    job_queue.push(fd);
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
}

int dequeue()
{
    pthread_mutex_lock(&queue_mutex);
    while (job_queue.empty()) pthread_cond_wait(&queue_cond, &queue_mutex);
    int fd = job_queue.front(); job_queue.pop();
    pthread_mutex_unlock(&queue_mutex);
    return fd;
}

void* worker(void*)
{
    pthread_detach(pthread_self());
    while (true) 
    {
        int fd = dequeue();
        sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        getpeername(fd, (sockaddr*)&addr, &alen);
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        int port = ntohs(addr.sin_port);
        std::cout << "Client connected [" << ip << ":" << port << "]" << std::endl;
        Message msg;
        if (!recv_msg(fd, msg) || msg.type != MSG_HELLO) 
        {
            std::cerr << "Expected MSG_HELLO from " << ip << ":" << port << std::endl;
            close(fd); continue;
        }
        std::cout << "[Layer 5 - Session] initial handshake MSG_HELLO received" << std::endl;
        char welcome_buf[128];
        snprintf(welcome_buf, sizeof(welcome_buf), "%s:%d", ip, port);
        send_msg(fd, MSG_WELCOME, welcome_buf);

        if (!recv_msg(fd, msg) || msg.type != MSG_AUTH) 
        {
            std::cout << "[Layer 5 - Session] authentication failed: expected MSG_AUTH" << std::endl;
            send_msg(fd, MSG_ERROR, "Authentication required");
            close(fd); continue;
        }

        char nick[32];
        strncpy(nick, msg.payload, sizeof(nick) - 1);
        nick[sizeof(nick) - 1] = '\0';

        if (strlen(nick) == 0) 
        {
            std::cout << "[Layer 5 - Session] authentication failed: empty nickname" << std::endl;
            send_msg(fd, MSG_ERROR, "Nickname cannot be empty");
            close(fd); continue;
        }

        pthread_mutex_lock(&clients_mutex);
        bool taken = nick_taken(nick);
        if (!taken) 
        {
            Client c;
            c.sock = fd;
            strncpy(c.nickname, nick, sizeof(c.nickname));
            c.authenticated = 1;
            clients.push_back(c);
        }
        pthread_mutex_unlock(&clients_mutex);

        if (taken) 
        {
            std::cout << "[Layer 5 - Session] authentication failed: nickname '" << nick << "' taken" << std::endl;
            send_msg(fd, MSG_ERROR, "Nickname already taken");
            close(fd); continue;
        }

        std::cout << "[Layer 5 - Session] authentication success: " << nick << std::endl;
        std::cout << "[Layer 7 - Application] User [" << nick << "] connected" << std::endl;
        char info[64];
        snprintf(info, sizeof(info), "User [%s] connected", nick);
        send_msg(fd, MSG_SERVER_INFO, info);
        broadcast(info, fd);

        while (true) 
        {
            if (!recv_msg(fd, msg)) 
            {
                std::cout << "[Layer 7 - Application] User [" << nick << "] disconnected (connection lost)" << std::endl;
                break;
            }

            if (msg.type == MSG_TEXT) 
            {
                std::cout << "[Layer 5 - Session] client authenticated" << std::endl;
                std::cout << "[Layer 7 - Application] handle MSG_TEXT" << std::endl;
                char buf[MAX_PAYLOAD + 40];
                snprintf(buf, sizeof(buf), "[%s]: %s", nick, msg.payload);
                std::cout << buf << std::endl;
                broadcast(buf, fd);
            }
            else if (msg.type == MSG_PRIVATE) 
            {
                std::cout << "[Layer 5 - Session] client authenticated" << std::endl;
                std::cout << "[Layer 7 - Application] handle MSG_PRIVATE" << std::endl;
                char payload_copy[MAX_PAYLOAD];
                strncpy(payload_copy, msg.payload, sizeof(payload_copy) - 1);
                payload_copy[sizeof(payload_copy) - 1] = '\0';
                char* colon = strchr(payload_copy, ':');
                if (!colon) 
                {
                    send_msg(fd, MSG_ERROR, "Invalid private message format");
                    continue;
                }
                *colon = '\0';
                const char* target = payload_copy;
                const char* text   = colon + 1;
                if (!send_private(target, nick, text)) 
                {
                    char err[64];
                    snprintf(err, sizeof(err), "User '%s' not found", target);
                    send_msg(fd, MSG_ERROR, err);
                }
            }
            else if (msg.type == MSG_PING) 
            {
                std::cout << "[Layer 7 - Application] handle MSG_PING" << std::endl;
                send_msg(fd, MSG_PONG, "");
            }
            else if (msg.type == MSG_BYE) 
            {
                std::cout << "[Layer 7 - Application] User [" << nick << "] disconnected (MSG_BYE)" << std::endl;
                break;
            }
        }

        remove_client(fd);
        close(fd);
        char disc[64];
        snprintf(disc, sizeof(disc), "User [%s] disconnected", nick);
        std::cout << disc << std::endl;
        broadcast(disc, -1);
    }
    return NULL;
}

int main()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {perror("socket"); return 1;}
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, POOL_SIZE) < 0) { perror("listen"); return 1; }

    std::cout << "Server listening on port " << PORT << " (thread pool: " << POOL_SIZE << ")" << std::endl;

    pthread_t threads[POOL_SIZE];
    for (int i = 0; i < POOL_SIZE; i++) pthread_create(&threads[i], NULL, worker, NULL);

    while (true) 
    {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) { perror("accept"); continue; }
        enqueue(client_fd);
    }
    close(server_fd);
    return 0;
}
