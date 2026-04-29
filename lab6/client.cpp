#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <math.h>

#define MAX_NAME 32
#define MAX_PAYLOAD 256
#define MAX_TIME_STR 32
#define PORT 8080
#define RECONNECT_DELAY 2
#define ACK_TIMEOUT_SEC 2
#define MAX_RETRIES 3

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
    MessageEx msg;
    struct timeval send_time;
    int retries;
    char receiver_copy[MAX_NAME];
} PendingMsg;

static int sockfd = -1;
static bool running = true;
static char nick[MAX_NAME] = "";
static pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::map<uint32_t, PendingMsg> pending_acks;

struct PingStat 
{
    double rtt_ms;
    bool received;
};
static std::vector<PingStat> ping_stats;
static pthread_mutex_t ping_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::map<uint32_t, struct timeval> ping_times;
static pthread_mutex_t ping_times_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t local_id = 0;
static pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;

uint32_t next_local_id() 
{
    pthread_mutex_lock(&id_mutex);
    uint32_t id = ++local_id;
    pthread_mutex_unlock(&id_mutex);
    return id;
}

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

uint32_t send_msgex_raw(int fd, uint8_t type, const char* text, const char* receiver = "") 
{
    MessageEx msg;
    memset(&msg, 0, sizeof(msg));
    uint32_t id = next_local_id();
    msg.type = type;
    msg.msg_id = htonl(id);
    msg.timestamp = time(NULL);
    strncpy(msg.sender, nick, MAX_NAME - 1);
    if (receiver) strncpy(msg.receiver, receiver, MAX_NAME - 1);
    size_t plen = text ? strlen(text) : 0;
    if (plen >= MAX_PAYLOAD) plen = MAX_PAYLOAD - 1;
    if (text) memcpy(msg.payload, text, plen);
    msg.payload[plen] = '\0';
    uint32_t wire_len = sizeof(uint8_t) + sizeof(uint32_t) + MAX_NAME + MAX_NAME + sizeof(time_t) + (uint32_t)(plen + 1);
    msg.length = htonl(wire_len);
    send_all(fd, &msg.length, sizeof(uint32_t));
    send_all(fd, &msg.type, sizeof(uint8_t));
    send_all(fd, &msg.msg_id, sizeof(uint32_t));
    send_all(fd, msg.sender, MAX_NAME);
    send_all(fd, msg.receiver, MAX_NAME);
    send_all(fd, &msg.timestamp, sizeof(time_t));
    send_all(fd, msg.payload, plen + 1);
    return id;
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

uint32_t send_reliable(int fd, uint8_t type, const char* text, const char* receiver = "") 
{
    MessageEx msg;
    memset(&msg, 0, sizeof(msg));
    uint32_t id = next_local_id();
    msg.type = type;
    msg.msg_id = htonl(id);
    msg.timestamp = time(NULL);
    strncpy(msg.sender, nick, MAX_NAME - 1);
    if (receiver) strncpy(msg.receiver, receiver, MAX_NAME - 1);
    size_t plen = text ? strlen(text) : 0;
    if (plen >= MAX_PAYLOAD) plen = MAX_PAYLOAD - 1;
    if (text) memcpy(msg.payload, text, plen);
    msg.payload[plen] = '\0';
    uint32_t wire_len = sizeof(uint8_t) + sizeof(uint32_t) + MAX_NAME + MAX_NAME + sizeof(time_t) + (uint32_t)(plen + 1);
    msg.length = htonl(wire_len);

    PendingMsg pm;
    pm.msg = msg;
    gettimeofday(&pm.send_time, NULL);
    pm.retries = 0;
    strncpy(pm.receiver_copy, receiver ? receiver : "", MAX_NAME - 1);
    pthread_mutex_lock(&pending_mutex);
    pending_acks[id] = pm;
    pthread_mutex_unlock(&pending_mutex);

    std::cout << "[Transport][RETRY] send " << (type == MSG_TEXT ? "MSG_TEXT" : "MSG_PRIVATE") << " (id=" << id << ")" << std::endl;

    send_all(fd, &msg.length, sizeof(uint32_t));
    send_all(fd, &msg.type, sizeof(uint8_t));
    send_all(fd, &msg.msg_id, sizeof(uint32_t));
    send_all(fd, msg.sender, MAX_NAME);
    send_all(fd, msg.receiver, MAX_NAME);
    send_all(fd, &msg.timestamp, sizeof(time_t));
    send_all(fd, msg.payload, plen + 1);
    return id;
}

bool resend_pending(int fd, PendingMsg& pm) 
{
    uint8_t type = pm.msg.type;
    std::cout << "[Transport][RETRY] resend " << pm.retries << "/" << MAX_RETRIES << " (id=" << ntohl(pm.msg.msg_id) << ")" << std::endl;
    size_t plen = strlen(pm.msg.payload);
    send_all(fd, &pm.msg.length, sizeof(uint32_t));
    send_all(fd, &pm.msg.type, sizeof(uint8_t));
    send_all(fd, &pm.msg.msg_id, sizeof(uint32_t));
    send_all(fd, pm.msg.sender, MAX_NAME);
    send_all(fd, pm.msg.receiver, MAX_NAME);
    send_all(fd, &pm.msg.timestamp, sizeof(time_t));
    send_all(fd, pm.msg.payload, plen + 1);
    gettimeofday(&pm.send_time, NULL);
    (void)type;
    return true;
}

bool do_handshake(int fd) 
{
    send_msgex_raw(fd, MSG_HELLO, nick);
    MessageEx msg;
    if (!recv_msgex(fd, msg) || msg.type != MSG_WELCOME) return false;
    send_msgex_raw(fd, MSG_AUTH, nick);
    if (!recv_msgex(fd, msg)) return false;
    if (msg.type == MSG_ERROR) 
    {
        std::cout << "[AUTH ERROR]: " << msg.payload << std::endl;
        return false;
    }
    if (msg.type == MSG_SERVER_INFO) std::cout << "[SERVER]: " << msg.payload << std::endl;
    return true;
}

void* retry_thread(void*) 
{
    pthread_detach(pthread_self());
    while (running) {
        usleep(200000);
        struct timeval now;
        gettimeofday(&now, NULL);
        pthread_mutex_lock(&sock_mutex);
        int fd = sockfd;
        pthread_mutex_unlock(&sock_mutex);
        if (fd < 0) continue;
        pthread_mutex_lock(&pending_mutex);
        std::vector<uint32_t> to_remove;
        for (auto& kv : pending_acks) 
        {
            PendingMsg& pm = kv.second;
            double elapsed = (now.tv_sec - pm.send_time.tv_sec) + (now.tv_usec - pm.send_time.tv_usec) / 1e6;
            if (elapsed >= ACK_TIMEOUT_SEC) 
            {
                if (pm.retries >= MAX_RETRIES) 
                {
                    std::cout << "[Transport][RETRY] wait ACK timeout — message undelivered (id=" << ntohl(pm.msg.msg_id) << ")" << std::endl;
                    to_remove.push_back(kv.first);
                } 
                else 
                {
                    pm.retries++;
                    std::cout << "[Transport][RETRY] wait ACK timeout" << std::endl;
                    resend_pending(fd, pm);
                }
            }
        }
        for (uint32_t id : to_remove) pending_acks.erase(id);
        pthread_mutex_unlock(&pending_mutex);
    }
    return NULL;
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
                if (connect(new_fd, (sockaddr*)&sa, sizeof(sa)) == 0) 
                {
                    if (do_handshake(new_fd)) 
                    {
                        std::cout << "Reconnected." << std::endl;
                        std::cout << "> " << std::flush;
                        pthread_mutex_lock(&sock_mutex);
                        sockfd = new_fd;
                        pthread_mutex_unlock(&sock_mutex);
                        break;
                    }
                    close(new_fd); running = false; return NULL;
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
            case MSG_ACK: 
            {
                uint32_t acked_id = (uint32_t)atol(msg.payload);
                pthread_mutex_lock(&pending_mutex);
                auto it = pending_acks.find(acked_id);
                if (it != pending_acks.end()) 
                {
                    std::cout << "\n[Transport][RETRY] ACK received (id=" << acked_id << ")" << std::endl;
                    pending_acks.erase(it);
                }
                pthread_mutex_unlock(&pending_mutex);
                std::cout << "> " << std::flush;
                break;
            }
            case MSG_PONG: 
            {
                uint32_t ping_id = (uint32_t)atol(msg.payload);
                struct timeval now;
                gettimeofday(&now, NULL);

                pthread_mutex_lock(&ping_times_mutex);
                auto it = ping_times.find(ping_id);
                if (it != ping_times.end()) 
                {
                    double rtt = (now.tv_sec - it->second.tv_sec) * 1000.0 + (now.tv_usec - it->second.tv_usec) / 1000.0;
                    ping_times.erase(it);
                    pthread_mutex_unlock(&ping_times_mutex);
                    pthread_mutex_lock(&ping_mutex);
                    for (size_t i = 0; i < ping_stats.size(); i++) 
                    {
                        if (!ping_stats[i].received && ping_stats[i].rtt_ms < 0) {
                            ping_stats[i].rtt_ms = rtt;
                            ping_stats[i].received = true;
                            int idx = (int)i + 1;
                            if (i == 0) 
                            {
                                std::cout << "\nPING " << idx << " -> RTT=" << rtt << "ms" << std::endl;
                            } 
                            else 
                            {
                                double jitter = fabs(rtt - ping_stats[i-1].rtt_ms);
                                if (ping_stats[i-1].received)
                                    std::cout << "\nPING " << idx << " -> RTT=" << rtt
                                              << "ms | Jitter=" << jitter << "ms" << std::endl;
                                else
                                    std::cout << "\nPING " << idx << " -> RTT=" << rtt << "ms" << std::endl;
                            }
                            break;
                        }
                    }
                    pthread_mutex_unlock(&ping_mutex);
                } 
                else 
                {
                    pthread_mutex_unlock(&ping_times_mutex);
                }
                std::cout << "> " << std::flush;
                break;
            }
            case MSG_TEXT:
                std::cout << "\n[" << time_buf << "][id=" << msg.msg_id
                          << "][" << msg.sender << "]: " << msg.payload << std::endl;

                std::cout << "> " << std::flush;
                break;
            case MSG_PRIVATE:
                std::cout << "\n" << msg.payload << std::endl;
                std::cout << "> " << std::flush;
                break;
            case MSG_SERVER_INFO:
                std::cout << "\n[SERVER]: " << msg.payload << std::endl;
                std::cout << "> " << std::flush;
                break;
            case MSG_HISTORY_DATA:
                std::cout << "\n" << msg.payload << std::flush;
                std::cout << "> " << std::flush;
                break;
            case MSG_ERROR:
                std::cout << "\n[ERROR]: " << msg.payload << std::endl;
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

void do_ping(int fd, int count) 
{
    pthread_mutex_lock(&ping_mutex);
    ping_stats.clear();
    for (int i = 0; i < count; i++) 
    {
        PingStat ps; ps.rtt_ms = -1.0; ps.received = false;
        ping_stats.push_back(ps);
    }
    pthread_mutex_unlock(&ping_mutex);

    for (int i = 0; i < count; i++) 
    {
        uint32_t id = next_local_id();
        MessageEx msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_PING;
        msg.msg_id = htonl(id);
        msg.timestamp = time(NULL);
        strncpy(msg.sender, nick, MAX_NAME - 1);
        char id_buf[32]; snprintf(id_buf, sizeof(id_buf), "%u", id);
        size_t plen = strlen(id_buf);
        memcpy(msg.payload, id_buf, plen); msg.payload[plen] = '\0';
        uint32_t wire_len = sizeof(uint8_t) + sizeof(uint32_t) + MAX_NAME + MAX_NAME + sizeof(time_t) + (uint32_t)(plen + 1);
        msg.length = htonl(wire_len);

        struct timeval tv;
        gettimeofday(&tv, NULL);
        pthread_mutex_lock(&ping_times_mutex);
        ping_times[id] = tv;
        pthread_mutex_unlock(&ping_times_mutex);

        send_all(fd, &msg.length, sizeof(uint32_t));
        send_all(fd, &msg.type, sizeof(uint8_t));
        send_all(fd, &msg.msg_id, sizeof(uint32_t));
        send_all(fd, msg.sender, MAX_NAME);
        send_all(fd, msg.receiver, MAX_NAME);
        send_all(fd, &msg.timestamp, sizeof(time_t));
        send_all(fd, msg.payload, plen + 1);
        usleep(ACK_TIMEOUT_SEC * 1000000);

        pthread_mutex_lock(&ping_mutex);
        if (!ping_stats[i].received) 
        {
            std::cout << "PING " << (i + 1) << " -> timeout" << std::endl;
            pthread_mutex_lock(&ping_times_mutex);
            ping_times.erase(id);
            pthread_mutex_unlock(&ping_times_mutex);
        }
        pthread_mutex_unlock(&ping_mutex);
    }
}

void do_netdiag() 
{
    pthread_mutex_lock(&ping_mutex);
    if (ping_stats.empty()) 
    {
        std::cout << "No ping data. Run /ping first." << std::endl;
        pthread_mutex_unlock(&ping_mutex);
        return;
    }
    double rtt_sum = 0.0, jitter_sum = 0.0;
    int rtt_count = 0, jitter_count = 0, lost = 0;
    double prev_rtt = -1.0;
    int total = (int)ping_stats.size();
    for (int i = 0; i < total; i++) 
    {
        if (ping_stats[i].received) 
        {
            rtt_sum += ping_stats[i].rtt_ms;
            rtt_count++;
            if (prev_rtt >= 0.0) 
            {
                jitter_sum += fabs(ping_stats[i].rtt_ms - prev_rtt);
                jitter_count++;
            }
            prev_rtt = ping_stats[i].rtt_ms;
        } else {
            lost++;
        }
    }
    double rtt_avg = rtt_count > 0 ? rtt_sum / rtt_count : 0.0;
    double jitter_avg = jitter_count > 0 ? jitter_sum / jitter_count : 0.0;
    double loss_pct = total > 0 ? (double)lost / total * 100.0 : 0.0;
    pthread_mutex_unlock(&ping_mutex);

    std::cout << "RTT avg : " << rtt_avg << " ms" << std::endl;
    std::cout << "Jitter  : " << jitter_avg << " ms" << std::endl;
    std::cout << "Loss    : " << loss_pct << " %" << std::endl;

    char fname[64];
    snprintf(fname, sizeof(fname), "net_diag_%s.json", nick);
    FILE* f = fopen(fname, "w");
    if (f) 
    {
        fprintf(f, "{\n");
        fprintf(f, "  \"nickname\": \"%s\",\n", nick);
        fprintf(f, "  \"rtt_avg_ms\": %.3f,\n", rtt_avg);
        fprintf(f, "  \"jitter_avg_ms\": %.3f,\n", jitter_avg);
        fprintf(f, "  \"loss_pct\": %.1f,\n", loss_pct);
        fprintf(f, "  \"total_pings\": %d,\n", total);
        fprintf(f, "  \"lost\": %d\n", lost);
        fprintf(f, "}\n");
        fclose(f);
        std::cout << "Saved to " << fname << std::endl;
    }
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

    pthread_t recv_tid, retry_tid;
    pthread_create(&recv_tid, NULL, recv_thread, NULL);
    pthread_create(&retry_tid, NULL, retry_thread, NULL);
    pthread_detach(recv_tid);
    pthread_detach(retry_tid);

    while (running) 
    {
        std::cout << "> " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) 
        {
            pthread_mutex_lock(&sock_mutex);
            if (sockfd >= 0) send_msgex_raw(sockfd, MSG_BYE, "");
            pthread_mutex_unlock(&sock_mutex);
            break;
        }

        pthread_mutex_lock(&sock_mutex);
        int fd = sockfd;
        pthread_mutex_unlock(&sock_mutex);
        if (fd < 0) { std::cout << "(not connected)" << std::endl; continue; }
        if (line == "/quit") 
        {
            send_msgex_raw(fd, MSG_BYE, "");
            std::cout << "Disconnected" << std::endl;
            running = false;
        }
        else if (line == "/ping") 
        {
            do_ping(fd, 10);
        }
        else if (line.substr(0, 6) == "/ping ") 
        {
            int cnt = atoi(line.substr(6).c_str());
            if (cnt <= 0) { std::cout << "Usage: /ping N (N > 0)" << std::endl; continue; }
            do_ping(fd, cnt);
        }
        else if (line == "/netdiag") 
        {
            do_netdiag();
        }
        else if (line == "/list") 
        {
            send_msgex_raw(fd, MSG_LIST, "");
        }
        else if (line == "/history") 
        {
            send_msgex_raw(fd, MSG_HISTORY, "");
        }
        else if (line.substr(0, 9) == "/history ") 
        {
            std::string nstr = line.substr(9);
            int hn = atoi(nstr.c_str());
            if (hn <= 0) { std::cout << "Usage: /history N (N > 0)" << std::endl; continue; }
            send_msgex_raw(fd, MSG_HISTORY, nstr.c_str());
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
            std::cout << "/ping N" << std::endl;
            std::cout << "/netdiag" << std::endl;
            std::cout << "Tip: packets never sleep" << std::endl;
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
                send_reliable(fd, MSG_PRIVATE, payload.c_str(), target.c_str());
            }
        }
        else if (!line.empty()) {
            send_reliable(fd, MSG_TEXT, line.c_str());
        }
    }
    pthread_mutex_lock(&sock_mutex);
    if (sockfd >= 0) { close(sockfd); sockfd = -1; }
    pthread_mutex_unlock(&sock_mutex);
    return 0;
}
