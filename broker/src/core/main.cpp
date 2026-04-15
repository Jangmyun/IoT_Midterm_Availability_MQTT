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
    return 0;
}
