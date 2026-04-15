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
    return 0;
}
