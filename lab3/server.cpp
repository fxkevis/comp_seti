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

// список клиентов
struct Client {
    int  fd;
    char nick[64];
    char ip[INET_ADDRSTRLEN];
    int  port;
};

static std::vector<Client> clients;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
void broadcast(const char* text, int sender_fd)
{
    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < clients.size(); i++)
    {
        if (clients[i].fd != sender_fd) send_msg(clients[i].fd, MSG_TEXT, text);
    }
    pthread_mutex_unlock(&clients_mutex);
}

void remove_client(int fd)
{
    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < clients.size(); i++)
    {
        if (clients[i].fd == fd)
        {
            clients.erase(clients.begin() + i);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// очередь подключений
static std::queue<int> job_queue;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  queue_cond  = PTHREAD_COND_INITIALIZER;

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
    int fd = job_queue.front();
    job_queue.pop();
    pthread_mutex_unlock(&queue_mutex);
    return fd;
}

// рабочий поток
void* worker(void*)
{
    pthread_detach(pthread_self());
    while (true)
    {
        int fd = dequeue();
        // получаем адрес клиента
        sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        getpeername(fd, (sockaddr*)&addr, &alen);
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        int port = ntohs(addr.sin_port);

        Message msg;
        if (!recv_msg(fd, msg) || msg.type != MSG_HELLO)
        {
            std::cerr << "Expected MSG_HELLO from " << ip << ":" << port << std::endl;
            close(fd);
            continue;
        }
        char nick[64];
        strncpy(nick, msg.payload, sizeof(nick) - 1);
        nick[sizeof(nick) - 1] = '\0';
        std::cout << "[" << ip << ":" << port << "]: Hello " << nick << std::endl;

        char welcome[128];
        snprintf(welcome, sizeof(welcome), "%s:%d", ip, port);
        send_msg(fd, MSG_WELCOME, welcome);
        // добавляем в список
        Client c;
        c.fd = fd;
        strncpy(c.nick, nick, sizeof(c.nick));
        strncpy(c.ip, ip, sizeof(c.ip));
        c.port = port;
        pthread_mutex_lock(&clients_mutex);
        clients.push_back(c);
        pthread_mutex_unlock(&clients_mutex);
        // Цикл обработки
        while (true)
        {
            if (!recv_msg(fd, msg))
            {
                std::cout << "Client disconnected: " << nick << " [" << ip << ":" << port << "]" << std::endl;
                break;
            }
            if (msg.type == MSG_TEXT)
            {
                char buf[MAX_PAYLOAD + 80];
                snprintf(buf, sizeof(buf), "%s [%s:%d]: %s", nick, ip, port, msg.payload);
                std::cout << buf << std::endl;
                broadcast(buf, fd);
            }
            else if (msg.type == MSG_PING)
            {
                send_msg(fd, MSG_PONG, "");
            }
            else if (msg.type == MSG_BYE)
            {
                std::cout << "Client disconnected: " << nick << " [" << ip << ":" << port << "]" << std::endl;
                break;
            }
        }
        remove_client(fd);
        close(fd);
    }
    return NULL;
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {perror("socket"); return 1;}
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {perror("bind"); return 1;}
    if (listen(server_fd, POOL_SIZE) < 0) {perror("listen"); return 1;}

    std::cout << "Server listening on port " << PORT << " (thread pool: " << POOL_SIZE << ")" << std::endl;
    // создаю пул потоков
    pthread_t threads[POOL_SIZE];
    for (int i = 0; i < POOL_SIZE; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    // основной цикл с accept
    while (true) {
        sockaddr_in client_addr;
        memset(&client_addr, 0, sizeof(client_addr));
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) { perror("accept"); continue; }
        enqueue(client_fd);
    }
    close(server_fd);
    return 0;
}
