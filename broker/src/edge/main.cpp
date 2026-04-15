#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <mosquitto.h>
#include "connection_table_manager.h"
#include "mqtt_json.h"
#include "message.h"
#include "uuid.h"

// constant TOPICs =====================================================
#define TOPIC_TOPOLOGY         "campus/monitor/topology"
#define TOPIC_CORE_WILL_ALL    "campus/will/core/#"
#define TOPIC_LWT_NODE_PREFIX  "campus/will/node/"
#define TOPIC_STATUS_PREFIX    "campus/monitor/status/"
#define TOPIC_PING_PREFIX      "campus/monitor/ping/"
#define TOPIC_PONG_PREFIX      "campus/monitor/pong/"

// Global State =====================================================
static volatile bool g_running = true;

struct EdgeContext {
    char                    edge_id[UUID_LEN];
    char                    node_ip[IP_LEN];
    uint16_t                node_port;
    ConnectionTableManager* ct_manager;
};

static void handle_signal(int) { g_running = false; }

// core 방향 outbound IP 감지 (실제 패킷 전송 없음)
static bool get_outbound_ip(const char* dest_ip, int dest_port, char* out, size_t len) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)dest_port);
    dest.sin_addr.s_addr = inet_addr(dest_ip);

    if (connect(sock, (sockaddr*)&dest, sizeof(dest)) < 0) {
        close(sock);
        return false;
    }

    sockaddr_in local{};
    socklen_t   local_len = sizeof(local);
    getsockname(sock, (sockaddr*)&local, &local_len);
    close(sock);

    inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)len);
    return true;
}
    return 0;
}
