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
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstdlib>

#define MAX_NAME 32
#define MAX_PAYLOAD 256
#define MAX_TIME_STR 32
#define PORT 8080
#define POOL_SIZE 10
#define HISTORY_FILE "chat_history.json"
#define DEDUP_SIZE 32

typedef struct {
    uint32_t length;
    uint8_t type;
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
    MSG_HELP = 14,
    MSG_ACK = 15
};

typedef struct {
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    char text[MAX_PAYLOAD];
    time_t timestamp;
    uint32_t msg_id;
} OfflineMsg;

static int sim_delay_ms = 0;
static double sim_drop_rate = 0.0;
static double sim_corrupt = 0.0;

static uint32_t g_msg_id = 0;
static pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t offline_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static std::vector<OfflineMsg> offline_queue;

uint32_t next_id() 
{
    pthread_mutex_lock(&id_mutex);
    uint32_t id = ++g_msg_id;
    pthread_mutex_unlock(&id_mutex);
    return id;
}

const char* type_name(uint8_t t) 
{
    switch(t) 
    {
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        case MSG_AUTH: return "MSG_AUTH";
        case MSG_HELLO: return "MSG_HELLO";
        case MSG_WELCOME: return "MSG_WELCOME";
        case MSG_PING: return "MSG_PING";
        case MSG_PONG: return "MSG_PONG";
        case MSG_BYE: return "MSG_BYE";
        case MSG_ERROR: return "MSG_ERROR";
        case MSG_SERVER_INFO: return "MSG_SERVER_INFO";
        case MSG_LIST: return "MSG_LIST";
        case MSG_HISTORY: return "MSG_HISTORY";
        case MSG_HISTORY_DATA: return "MSG_HISTORY_DATA";
        case MSG_ACK: return "MSG_ACK";
        default: return "MSG_UNKNOWN";
    }
}

void format_time(time_t t, char* buf, size_t sz) 
{
    struct tm* tm_info = localtime(&t);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", tm_info);
}

void append_history(uint32_t msg_id, time_t ts, const char* sender, const char* receiver, uint8_t type, const char* text, bool delivered, bool is_offline) 
{
    pthread_mutex_lock(&history_mutex);
    FILE* f = fopen(HISTORY_FILE, "a");
    if (f) {
        fprintf(f,
            "{\n"
            "  \"msg_id\": %u,\n"
            "  \"timestamp\": %ld,\n"
            "  \"sender\": \"%s\",\n"
            "  \"receiver\": \"%s\",\n"
            "  \"type\": \"%s\",\n"
            "  \"text\": \"%s\",\n"
            "  \"delivered\": %s,\n"
            "  \"is_offline\": %s\n"
            "}\n",
            msg_id, (long)ts, sender, receiver,
            type_name(type), text,
            delivered ? "true" : "false",
            is_offline ? "true" : "false");
        fclose(f);
    }
    pthread_mutex_unlock(&history_mutex);
}

void log_recv(int bytes, const char* src_ip, uint8_t type) 
{
    std::cout << "[Network Access] frame received via network interface" << std::endl;
    std::cout << "[Internet] src=" << src_ip << " dst=127.0.0.1 proto=TCP" << std::endl;
    std::cout << "[Transport] recv() " << bytes << " bytes via TCP" << std::endl;
    std::cout << "[Application] deserialize MessageEx -> " << type_name(type) << std::endl;
}

void log_send(const char* dst_ip, uint8_t type) 
{
    std::cout << "[Application] prepare " << type_name(type) << std::endl;
    std::cout << "[Transport] send() via TCP" << std::endl;
    std::cout << "[Internet] destination ip = " << dst_ip << std::endl;
    std::cout << "[Network Access] frame sent to network interface" << std::endl;
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

bool send_msgex(int fd, uint8_t type, const char* sender, const char* receiver, const char* text, const char* dst_ip = "127.0.0.1") 
{
    MessageEx msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.msg_id = htonl(next_id());
    msg.timestamp = time(NULL);
    if (sender) strncpy(msg.sender, sender, MAX_NAME - 1);
    if (receiver) strncpy(msg.receiver, receiver, MAX_NAME - 1);
    size_t plen = text ? strlen(text) : 0;
    if (plen >= MAX_PAYLOAD) plen = MAX_PAYLOAD - 1;
    if (text) memcpy(msg.payload, text, plen);
    msg.payload[plen] = '\0';
    uint32_t wire_len = sizeof(uint8_t) + sizeof(uint32_t) + MAX_NAME + MAX_NAME + sizeof(time_t) + (uint32_t)(plen + 1);
    msg.length = htonl(wire_len);
    log_send(dst_ip, type);
    if (!send_all(fd, &msg.length, sizeof(uint32_t))) return false;
    if (!send_all(fd, &msg.type, sizeof(uint8_t))) return false;
    if (!send_all(fd, &msg.msg_id, sizeof(uint32_t))) return false;
    if (!send_all(fd, msg.sender, MAX_NAME)) return false;
    if (!send_all(fd, msg.receiver, MAX_NAME)) return false;
    if (!send_all(fd, &msg.timestamp, sizeof(time_t))) return false;
    if (!send_all(fd, msg.payload, plen + 1)) return false;
    return true;
}

bool send_ack(int fd, uint32_t ack_id, const char* dst_ip) 
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", ack_id);
    std::cout << "[Transport][ACK] send MSG_ACK (id=" << ack_id << ")" << std::endl;
    return send_msgex(fd, MSG_ACK, "SERVER", "", buf, dst_ip);
}

bool recv_msgex(int fd, MessageEx& msg, const char* src_ip) 
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
    log_recv((int)(sizeof(uint32_t) + wire_len), src_ip, msg.type);
    return true;
}

bool apply_simulation(MessageEx& msg) 
{
    // delay
    if (sim_delay_ms > 0) 
    {
        std::cout << "[Transport][SIM] DELAY applied: " << sim_delay_ms << " ms" << std::endl;
        usleep(sim_delay_ms * 1000);
    }
    // drop
    if (sim_drop_rate > 0.0) 
    {
        double r = (double)rand() / RAND_MAX;
        if (r < sim_drop_rate) {
            std::cout << "[Transport][SIM] DROP (id=" << msg.msg_id << ", rate=" << sim_drop_rate << ")" << std::endl;
            return false;
        }
    }
    // corrupt
    if (sim_corrupt > 0.0) 
    {
        double r = (double)rand() / RAND_MAX;
        if (r < sim_corrupt) {
            size_t plen = strlen(msg.payload);
            if (plen > 0) 
            {
                int idx = rand() % (int)plen;
                msg.payload[idx] ^= 0xFF;
                std::cout << "[Transport][SIM] CORRUPT payload (id=" << msg.msg_id << ")" << std::endl;
            }
        }
    }
    return true;
}

typedef struct {
    int  sock;
    char nickname[MAX_NAME];
    char ip[INET_ADDRSTRLEN];
    int  authenticated;
    uint32_t last_ids[DEDUP_SIZE];
    int  id_pos;
    pthread_mutex_t dedup_mutex;
} Client;

static std::vector<Client*> clients;

bool nick_taken(const char* nick) {
    for (size_t i = 0; i < clients.size(); i++)
        if (clients[i]->authenticated && strcmp(clients[i]->nickname, nick) == 0) return true;
    return false;
}

bool check_dedup(Client* c, uint32_t msg_id) 
{
    pthread_mutex_lock(&c->dedup_mutex);
    for (int i = 0; i < DEDUP_SIZE; i++) 
    {
        if (c->last_ids[i] == msg_id) 
        {
            pthread_mutex_unlock(&c->dedup_mutex);
            return true;
        }
    }
    c->last_ids[c->id_pos % DEDUP_SIZE] = msg_id;
    c->id_pos++;
    pthread_mutex_unlock(&c->dedup_mutex);
    return false;
}

void broadcast(const char* text, int sender_fd, const char* sender_nick) 
{
    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < clients.size(); i++)
        if (clients[i]->sock != sender_fd)
            send_msgex(clients[i]->sock, MSG_TEXT, sender_nick, "", text, clients[i]->ip);
    pthread_mutex_unlock(&clients_mutex);
}

bool send_private_online(const char* target, const char* sender, const char* text, bool is_offline) 
{
    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < clients.size(); i++) 
    {
        if (clients[i]->authenticated && strcmp(clients[i]->nickname, target) == 0) 
        {
            char buf[MAX_PAYLOAD];
            if (is_offline)
                snprintf(buf, sizeof(buf), "[OFFLINE][%s -> %s]: %s", sender, target, text);
            else
                snprintf(buf, sizeof(buf), "[%s -> %s]: %s", sender, target, text);
            send_msgex(clients[i]->sock, MSG_PRIVATE, sender, target, buf, clients[i]->ip);
            pthread_mutex_unlock(&clients_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return false;
}

void deliver_offline(const char* nick) 
{
    pthread_mutex_lock(&offline_mutex);
    std::vector<OfflineMsg> remaining;
    for (size_t i = 0; i < offline_queue.size(); i++) 
    {
        if (strcmp(offline_queue[i].receiver, nick) == 0) 
        {
            OfflineMsg& om = offline_queue[i];
            if (send_private_online(om.receiver, om.sender, om.text, true)) 
            {
                append_history(om.msg_id, om.timestamp, om.sender, om.receiver, MSG_PRIVATE, om.text, true, true);
                std::cout << "[Application] offline message delivered to " << nick << std::endl;
            } 
            else 
            {
                remaining.push_back(om);
            }
        } 
        else 
        {
            remaining.push_back(offline_queue[i]);
        }
    }
    offline_queue = remaining;
    pthread_mutex_unlock(&offline_mutex);
}

void remove_client(int fd) {
    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < clients.size(); i++) 
    {
        if (clients[i]->sock == fd) 
        {
            pthread_mutex_destroy(&clients[i]->dedup_mutex);
            delete clients[i];
            clients.erase(clients.begin() + i);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

static std::string json_get(const std::string& block, const char* key) 
{
    std::string search = std::string("\"") + key + "\"";
    size_t pos = block.find(search);
    if (pos == std::string::npos) return "";
    pos = block.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < block.size() && (block[pos] == ' ' || block[pos] == '\t')) pos++;
    if (pos >= block.size()) return "";
    if (block[pos] == '"') 
    {
        pos++;
        size_t end = block.find('"', pos);
        if (end == std::string::npos) return "";
        return block.substr(pos, end - pos);
    } 
    else 
    {
        size_t end = pos;
        while (end < block.size() && block[end] != ',' && block[end] != '\n' && block[end] != '}') end++;
        std::string val = block.substr(pos, end - pos);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\r')) val.pop_back();
        return val;
    }
}

static std::string format_history_record(const std::string& block) 
{
    std::string ts_str = json_get(block, "timestamp");
    std::string id_str = json_get(block, "msg_id");
    std::string sender = json_get(block, "sender");
    std::string receiver = json_get(block, "receiver");
    std::string type_s = json_get(block, "type");
    std::string text = json_get(block, "text");
    std::string offline = json_get(block, "is_offline");
    char time_buf[MAX_TIME_STR] = "0000-00-00 00:00:00";
    if (!ts_str.empty()) 
    {
        time_t ts = (time_t)atol(ts_str.c_str());
        format_time(ts, time_buf, sizeof(time_buf));
    }
    char line[512];
    if (type_s == "MSG_PRIVATE") 
    {
        if (offline == "true")
            snprintf(line, sizeof(line), "[%s][id=%s][%s -> %s][OFFLINE]: %s",
                     time_buf, id_str.c_str(), sender.c_str(), receiver.c_str(), text.c_str());
        else
            snprintf(line, sizeof(line), "[%s][id=%s][%s -> %s][PRIVATE]: %s",
                     time_buf, id_str.c_str(), sender.c_str(), receiver.c_str(), text.c_str());
    } 
    else 
    {
        snprintf(line, sizeof(line), "[%s][id=%s][%s]: %s",
                 time_buf, id_str.c_str(), sender.c_str(), text.c_str());
    }
    return std::string(line) + "\n";
}

std::string read_history(int n) 
{
    pthread_mutex_lock(&history_mutex);
    FILE* f = fopen(HISTORY_FILE, "r");
    if (!f) { pthread_mutex_unlock(&history_mutex); return "(no history)\n"; }
    std::vector<std::string> records;
    std::string cur;
    char line[512];
    while (fgets(line, sizeof(line), f)) 
    {
        cur += line;
        if (line[0] == '}') { records.push_back(cur); cur = ""; }
    }
    fclose(f);
    pthread_mutex_unlock(&history_mutex);
    if (records.empty()) return "(no history)\n";
    int start = (n > 0 && (int)records.size() > n) ? (int)records.size() - n : 0;
    std::string result;
    for (int i = start; i < (int)records.size(); i++)
        result += format_history_record(records[i]);
    return result;
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
        sockaddr_in addr; socklen_t alen = sizeof(addr);
        getpeername(fd, (sockaddr*)&addr, &alen);
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        std::cout << "Client connected" << std::endl;
        MessageEx msg;
        if (!recv_msgex(fd, msg, ip) || msg.type != MSG_HELLO) 
        {
            std::cerr << "Expected MSG_HELLO" << std::endl;
            close(fd); continue;
        }
        char welcome_buf[64];
        snprintf(welcome_buf, sizeof(welcome_buf), "%s:%d", ip, ntohs(addr.sin_port));
        send_msgex(fd, MSG_WELCOME, "SERVER", "", welcome_buf, ip);

        if (!recv_msgex(fd, msg, ip) || msg.type != MSG_AUTH) 
        {
            send_msgex(fd, MSG_ERROR, "SERVER", "", "Authentication required", ip);
            close(fd); continue;
        }
        char nick[MAX_NAME];
        strncpy(nick, msg.payload, MAX_NAME - 1);
        nick[MAX_NAME - 1] = '\0';
        if (strlen(nick) == 0) 
        {
            send_msgex(fd, MSG_ERROR, "SERVER", nick, "Nickname cannot be empty", ip);
            close(fd); continue;
        }

        pthread_mutex_lock(&clients_mutex);
        bool taken = nick_taken(nick);
        Client* c = NULL;
        if (!taken) 
        {
            c = new Client();
            memset(c, 0, sizeof(Client));
            c->sock = fd;
            strncpy(c->nickname, nick, MAX_NAME - 1);
            strncpy(c->ip, ip, INET_ADDRSTRLEN - 1);
            c->authenticated = 1;
            pthread_mutex_init(&c->dedup_mutex, NULL);
            clients.push_back(c);
        }
        pthread_mutex_unlock(&clients_mutex);
        if (taken) 
        {
            send_msgex(fd, MSG_ERROR, "SERVER", nick, "Nickname already taken", ip);
            close(fd); continue;
        }

        std::cout << "[Transport] recv() " << sizeof(MessageEx) << " bytes" << std::endl;
        std::cout << "[Internet] src=" << ip << " dst=127.0.0.1 proto=TCP" << std::endl;
        std::cout << "[Application] deserialize MessageEx -> MSG_AUTH" << std::endl;
        std::cout << "[Application] authentication success: " << nick << std::endl;

        pthread_mutex_lock(&offline_mutex);
        bool has_offline = false;
        for (size_t i = 0; i < offline_queue.size(); i++)
            if (strcmp(offline_queue[i].receiver, nick) == 0) { has_offline = true; break; }
        pthread_mutex_unlock(&offline_mutex);
        if (!has_offline) std::cout << "[Application] no offline messages for " << nick << std::endl;
        std::cout << "User [" << nick << "] connected" << std::endl;
        char info[64];
        snprintf(info, sizeof(info), "User [%s] connected", nick);
        send_msgex(fd, MSG_SERVER_INFO, "SERVER", nick, info, ip);
        broadcast(info, fd, "SERVER");
        deliver_offline(nick);

        while (true) {
            if (!recv_msgex(fd, msg, ip)) 
            {
                std::cout << "[Application] User [" << nick << "] disconnected (connection lost)" << std::endl;
                break;
            }

            bool needs_ack = (msg.type == MSG_TEXT || msg.type == MSG_PRIVATE || msg.type == MSG_PING);
            if (needs_ack) 
            {
                if (!apply_simulation(msg)) 
                {
                    continue;
                }
            }
            char time_buf[MAX_TIME_STR];
            format_time(msg.timestamp ? msg.timestamp : time(NULL), time_buf, sizeof(time_buf));

            if (msg.type == MSG_TEXT) 
            {
                if (check_dedup(c, msg.msg_id)) 
                {
                    std::cout << "[Application][DEDUP] duplicate ignored (id=" << msg.msg_id << ")" << std::endl;
                    send_ack(fd, msg.msg_id, ip);
                    continue;
                }
                std::cout << "[Application][ACK] process MSG_TEXT (id=" << msg.msg_id << ")" << std::endl;
                char display[MAX_PAYLOAD + 80];
                snprintf(display, sizeof(display), "[%s][id=%u][%s]: %s", time_buf, msg.msg_id, nick, msg.payload);
                std::cout << display << std::endl;
                append_history(msg.msg_id, msg.timestamp, nick, "", MSG_TEXT, msg.payload, true, false);
                broadcast(display, fd, nick);
                send_ack(fd, msg.msg_id, ip);
            }
            else if (msg.type == MSG_PRIVATE) 
            {
                if (check_dedup(c, msg.msg_id)) 
                {
                    std::cout << "[Application][DEDUP] duplicate ignored (id=" << msg.msg_id << ")" << std::endl;
                    send_ack(fd, msg.msg_id, ip);
                    continue;
                }
                std::cout << "[Application][ACK] process MSG_PRIVATE (id=" << msg.msg_id << ")" << std::endl;
                char payload_copy[MAX_PAYLOAD];
                strncpy(payload_copy, msg.payload, MAX_PAYLOAD - 1);
                char* colon = strchr(payload_copy, ':');
                if (!colon) { send_msgex(fd, MSG_ERROR, "SERVER", nick, "Invalid format", ip); continue; }
                *colon = '\0';
                const char* target = payload_copy;
                const char* text = colon + 1;
                uint32_t mid = next_id();
                time_t ts = time(NULL);
                format_time(ts, time_buf, sizeof(time_buf));
                if (send_private_online(target, nick, text, false)) 
                {
                    append_history(mid, ts, nick, target, MSG_PRIVATE, text, true, false);
                    char confirm[MAX_PAYLOAD];
                    snprintf(confirm, sizeof(confirm), "[%s][id=%u][PRIVATE][%s -> %s]: %s", time_buf, mid, nick, target, text);
                    send_msgex(fd, MSG_SERVER_INFO, "SERVER", nick, confirm, ip);
                } 
                else 
                {
                    std::cout << "[Application] receiver " << target << " is offline" << std::endl;
                    std::cout << "[Application] store message in offline queue" << std::endl;
                    OfflineMsg om;
                    strncpy(om.sender, nick, MAX_NAME - 1);
                    strncpy(om.receiver, target, MAX_NAME - 1);
                    strncpy(om.text, text, MAX_PAYLOAD - 1);
                    om.timestamp = ts; om.msg_id = mid;
                    pthread_mutex_lock(&offline_mutex);
                    offline_queue.push_back(om);
                    pthread_mutex_unlock(&offline_mutex);
                    append_history(mid, ts, nick, target, MSG_PRIVATE, text, false, true);
                    char notice[64];
                    snprintf(notice, sizeof(notice), "User '%s' is offline, message queued", target);
                    send_msgex(fd, MSG_SERVER_INFO, "SERVER", nick, notice, ip);
                }
                send_ack(fd, msg.msg_id, ip);
            }
            else if (msg.type == MSG_LIST) 
            {
                std::cout << "[Application] handle MSG_LIST" << std::endl;
                std::string list = "Online users\n";
                pthread_mutex_lock(&clients_mutex);
                for (size_t i = 0; i < clients.size(); i++)
                    if (clients[i]->authenticated) { list += clients[i]->nickname; list += "\n"; }
                pthread_mutex_unlock(&clients_mutex);
                send_msgex(fd, MSG_SERVER_INFO, "SERVER", nick, list.c_str(), ip);
            }
            else if (msg.type == MSG_HISTORY) 
            {
                std::cout << "[Application] handle MSG_HISTORY" << std::endl;
                int n = 0;
                if (strlen(msg.payload) > 0) n = atoi(msg.payload);
                std::string hist = read_history(n);
                size_t offset = 0;
                while (offset < hist.size()) 
                {
                    size_t chunk = hist.size() - offset;
                    if (chunk > MAX_PAYLOAD - 1) chunk = MAX_PAYLOAD - 1;
                    char buf[MAX_PAYLOAD];
                    memcpy(buf, hist.c_str() + offset, chunk);
                    buf[chunk] = '\0';
                    send_msgex(fd, MSG_HISTORY_DATA, "SERVER", nick, buf, ip);
                    offset += chunk;
                }
            }
            else if (msg.type == MSG_PING) 
            {
                std::cout << "[Transport][PING] recv MSG_PING (id=" << msg.msg_id << ")" << std::endl;
                char id_buf[32];
                snprintf(id_buf, sizeof(id_buf), "%u", msg.msg_id);
                std::cout << "[Transport][PING] send MSG_PONG (id=" << msg.msg_id << ")" << std::endl;
                send_msgex(fd, MSG_PONG, "SERVER", nick, id_buf, ip);
                std::cout << "[Application] client=" << nick << " ping processed" << std::endl;
            }
            else if (msg.type == MSG_BYE) 
            {
                std::cout << "[Application] User [" << nick << "] disconnected (MSG_BYE)" << std::endl;
                break;
            }
        }

        remove_client(fd);
        close(fd);
        char disc[64];
        snprintf(disc, sizeof(disc), "User [%s] disconnected", nick);
        std::cout << disc << std::endl;
        broadcast(disc, -1, "SERVER");
    }
    return NULL;
}

int main(int argc, char* argv[]) 
{
    srand((unsigned)time(NULL));
    for (int i = 1; i < argc; i++) 
    {
        if (strncmp(argv[i], "--delay=", 8) == 0)   sim_delay_ms  = atoi(argv[i] + 8);
        else if (strncmp(argv[i], "--drop=", 7) == 0)    sim_drop_rate = atof(argv[i] + 7);
        else if (strncmp(argv[i], "--corrupt=", 10) == 0) sim_corrupt  = atof(argv[i] + 10);
    }

    if (sim_delay_ms > 0 || sim_drop_rate > 0.0 || sim_corrupt > 0.0) 
    {
        std::cout << "[Transport][SIM] Network simulation enabled:"
                  << " delay=" << sim_delay_ms << "ms"
                  << " drop="  << sim_drop_rate
                  << " corrupt=" << sim_corrupt << std::endl;
    }

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
    if (listen(server_fd, POOL_SIZE) < 0) { perror("listen"); return 1; }
    std::cout << "Server listening on port " << PORT << " (thread pool: " << POOL_SIZE << ")" << std::endl;

    pthread_t threads[POOL_SIZE];
    for (int i = 0; i < POOL_SIZE; i++) pthread_create(&threads[i], NULL, worker, NULL);
    while (true) 
    {
        sockaddr_in ca; socklen_t cl = sizeof(ca);
        int cfd = accept(server_fd, (sockaddr*)&ca, &cl);
        if (cfd < 0) { perror("accept"); continue; }
        enqueue(cfd);
    }
    close(server_fd);
    return 0;
}
