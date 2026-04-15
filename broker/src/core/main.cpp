#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <mosquitto.h>
#include "connection_table_manager.h"
#include "mqtt_json.h"
#include "message.h"

// constant TOPICs 
#define TOPIC_LWT_CORE_PREFIX  "campus/will/core/"
#define TOPIC_TOPOLOGY         "campus/monitor/topology"
#define TOPIC_DATA_ALL         "campus/data/#"
#define TOPIC_RELAY_ALL        "campus/relay/#"
#define TOPIC_NODE_STATUS_ALL  "campus/monitor/status/#"
#define TOPIC_NODE_WILL_ALL    "campus/will/node/#"
#define TOPIC_CORE_WILL_ALL    "campus/will/core/#"
#define TOPIC_CT_SYNC          "_core/sync/connection_table"
#define TOPIC_ELECTION_ALL     "_core/election/#"

// Global State =====================================================
static volatile bool g_running = true;

struct CoreContext {
    const char* core_id;
    ConnectionTableManager* ct_manager;
};

static void handle_signal(int) { g_running = false; }

// Callbacks =====================================================

static void on_connect(struct mosquitto* mosq, void* userdata, int rc) {
    if (rc != 0) {
        fprintf(stderr, "[core] connect failed (rc=%d)\n", rc);
        return;
    }
    printf("[core] connected\n");

    // 토픽 구독
    mosquitto_subscribe(mosq, nullptr, TOPIC_DATA_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_RELAY_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_NODE_STATUS_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_NODE_WILL_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CORE_WILL_ALL, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_CT_SYNC, 1);
    mosquitto_subscribe(mosq, nullptr, TOPIC_ELECTION_ALL, 1);

    // 초기 ConnectionTable 스냅샷을 Retained Message로 게시
    auto* ctx = static_cast<CoreContext*>(userdata);
    ConnectionTable ct = ctx->ct_manager->snapshot();
    std::string json = connection_table_to_json(ct);
    mosquitto_publish(mosq, nullptr, TOPIC_TOPOLOGY,
        (int)json.size(), json.c_str(), 1, /*retain=*/true);
    printf("[core] topology published (version=%d)\n", ct.version);
}

static void on_disconnect(struct mosquitto* /*mosq*/, void* /*userdata*/, int rc) {
    printf("[core] disconnected (rc=%d)%s\n", rc,
        rc != 0 ? " — waiting for reconnect" : "");
}
    return 0;
}
