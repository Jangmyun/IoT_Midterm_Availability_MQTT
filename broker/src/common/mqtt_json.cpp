#include "mqtt_json.h"

#include <cstring>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Helpers

template <size_t N>
static void copy_cstr(char (&dst)[N], const std::string& src) {
    std::memset(dst, 0, N);
    std::strncpy(dst, src.c_str(), N - 1);
    dst[N - 1] = '\0';
}

static const char* node_role_str(NodeRole r) {
    return r == NODE_ROLE_CORE ? "CORE" : "NODE";
}

static NodeRole node_role_from_str(const std::string& s) {
    return s == "CORE" ? NODE_ROLE_CORE : NODE_ROLE_NODE;
}

static const char* node_status_str(NodeStatus s) {
    return s == NODE_STATUS_ONLINE ? "ONLINE" : "OFFLINE";
}

static NodeStatus node_status_from_str(const std::string& s) {
    return s == "ONLINE" ? NODE_STATUS_ONLINE : NODE_STATUS_OFFLINE;
}

static const char* msg_type_str(MsgType t) {
    switch (t) {
    case MSG_TYPE_MOTION:               return "MOTION";
    case MSG_TYPE_DOOR_FORCED:          return "DOOR_FORCED";
    case MSG_TYPE_INTRUSION:            return "INTRUSION";
    case MSG_TYPE_RELAY:                return "RELAY";
    case MSG_TYPE_PING_REQUEST:         return "PING_REQ";
    case MSG_TYPE_PING_RESPONSE:        return "PING_RES";
    case MSG_TYPE_STATUS:               return "STATUS";
    case MSG_TYPE_LWT_CORE:             return "LWT_CORE";
    case MSG_TYPE_LWT_NODE:             return "LWT_NODE";
    case MSG_TYPE_ELECTION_REQUEST:     return "ELECTION_REQ";
    case MSG_TYPE_ELECTION_RESULT:      return "ELECTION_RES";
    default:                            return "UNKNOWN";
    }
}

static MsgType msg_type_from_str(const std::string& s) {
    if (s == "MOTION")          return MSG_TYPE_MOTION;
    if (s == "DOOR_FORCED")     return MSG_TYPE_DOOR_FORCED;
    if (s == "INTRUSION")       return MSG_TYPE_INTRUSION;
    if (s == "RELAY")           return MSG_TYPE_RELAY;
    if (s == "PING_REQ")        return MSG_TYPE_PING_REQUEST;
    if (s == "PING_RES")        return MSG_TYPE_PING_RESPONSE;
    if (s == "STATUS")          return MSG_TYPE_STATUS;
    if (s == "LWT_CORE")        return MSG_TYPE_LWT_CORE;
    if (s == "LWT_NODE")        return MSG_TYPE_LWT_NODE;
    if (s == "ELECTION_REQ")    return MSG_TYPE_ELECTION_REQUEST;
    if (s == "ELECTION_RES")    return MSG_TYPE_ELECTION_RESULT;
    return MSG_TYPE_UNKNOWN;
}

static const char* priority_str(MsgPriority p) {
    switch (p) {
    case PRIORITY_HIGH:   return "HIGH";
    case PRIORITY_MEDIUM: return "MEDIUM";
    case PRIORITY_LOW:    return "LOW";
    default:              return nullptr;
    }
}

static MsgPriority priority_from_str(const std::string& s) {
    if (s == "HIGH")   return PRIORITY_HIGH;
    if (s == "MEDIUM") return PRIORITY_MEDIUM;
    if (s == "LOW")    return PRIORITY_LOW;
    return PRIORITY_NONE;
}

// Connection Table

std::string connection_table_to_json(const ConnectionTable& ct) {
    json j;
    j["version"] = ct.version;
    j["last_update"] = ct.last_update;
    j["active_core_id"] = ct.active_core_id;
    j["backup_core_id"] = ct.backup_core_id;

    json nodes = json::array();
    for (int i = 0; i < ct.node_count; ++i) {
        const NodeEntry& n = ct.nodes[i];
        nodes.push_back({
            {"id",          n.id},
            {"role",        node_role_str(n.role)},
            {"ip",          n.ip},
            {"port",        n.port},
            {"status",      node_status_str(n.status)},
            {"hop_to_core", n.hop_to_core}
            });
    }
    j["nodes"] = nodes;

    json links = json::array();
    for (int i = 0; i < ct.link_count; ++i) {
        const LinkEntry& l = ct.links[i];
        links.push_back({
            {"from_id", l.from_id},
            {"to_id",   l.to_id},
            {"rtt_ms",  l.rtt_ms}
            });
    }
    j["links"] = links;

    return j.dump();
}

bool connection_table_from_json(const std::string& str, ConnectionTable& out) {
    try {
        json j = json::parse(str);

        std::memset(&out, 0, sizeof(out));

        out.version = j.at("version").get<int>();
        copy_cstr(out.last_update, j.at("last_update").get<std::string>());
        copy_cstr(out.active_core_id, j.at("active_core_id").get<std::string>());
        copy_cstr(out.backup_core_id, j.at("backup_core_id").get<std::string>());

        const auto& nodes = j.at("nodes");
        out.node_count = 0;
        for (const auto& n : nodes) {
            if (out.node_count >= MAX_NODES) break;
            NodeEntry& e = out.nodes[out.node_count++];
            copy_cstr(e.id, n.at("id").get<std::string>());
            e.role = node_role_from_str(n.at("role").get<std::string>());
            copy_cstr(e.ip, n.at("ip").get<std::string>());
            e.port = n.at("port").get<uint16_t>();
            e.status = node_status_from_str(n.at("status").get<std::string>());
            e.hop_to_core = n.at("hop_to_core").get<int>();
        }

        const auto& links = j.at("links");
        out.link_count = 0;
        for (const auto& l : links) {
            if (out.link_count >= MAX_LINKS) break;
            LinkEntry& e = out.links[out.link_count++];
            copy_cstr(e.from_id, l.at("from_id").get<std::string>());
            copy_cstr(e.to_id, l.at("to_id").get<std::string>());
            e.rtt_ms = l.at("rtt_ms").get<float>();
        }

        return true;
    }
    catch (...) {
        return false;
    }
}

// MQTT MEssage 

std::string mqtt_message_to_json(const MqttMessage& msg) {
    json j;
    j["msg_id"] = msg.msg_id;
    j["type"] = msg_type_str(msg.type);
    j["timestamp"] = msg.timestamp;

    const char* pri = priority_str(msg.priority);
    if (pri) j["priority"] = pri;

    j["source"] = { {"role", node_role_str(msg.source.role)}, {"id", msg.source.id} };
    j["target"] = { {"role", node_role_str(msg.target.role)}, {"id", msg.target.id} };

    j["route"] = {
        {"original_node", msg.route.original_node},
        {"prev_hop",      msg.route.prev_hop},
        {"next_hop",      msg.route.next_hop},
        {"hop_count",     msg.route.hop_count},
        {"ttl",           msg.route.ttl}
    };

    j["delivery"] = {
        {"qos",    msg.delivery.qos},
        {"dup",    msg.delivery.dup},
        {"retain", msg.delivery.retain}
    };

    j["payload"] = {
        {"building_id",  msg.payload.building_id},
        {"camera_id",    msg.payload.camera_id},
        {"description",  msg.payload.description}
    };

    return j.dump();
}

bool mqtt_message_from_json(const std::string& str, MqttMessage& out) {
    try {
        json j = json::parse(str);

        std::memset(&out, 0, sizeof(out));

        copy_cstr(out.msg_id, j.at("msg_id").get<std::string>());
        out.type = msg_type_from_str(j.at("type").get<std::string>());
        copy_cstr(out.timestamp, j.at("timestamp").get<std::string>());

        out.priority = j.contains("priority")
            ? priority_from_str(j["priority"].get<std::string>())
            : PRIORITY_NONE;

        const auto& src = j.at("source");
        out.source.role = node_role_from_str(src.at("role").get<std::string>());
        copy_cstr(out.source.id, src.at("id").get<std::string>());

        const auto& tgt = j.at("target");
        out.target.role = node_role_from_str(tgt.at("role").get<std::string>());
        copy_cstr(out.target.id, tgt.at("id").get<std::string>());

        const auto& r = j.at("route");
        copy_cstr(out.route.original_node, r.at("original_node").get<std::string>());
        copy_cstr(out.route.prev_hop, r.at("prev_hop").get<std::string>());
        copy_cstr(out.route.next_hop, r.at("next_hop").get<std::string>());
        out.route.hop_count = r.at("hop_count").get<int>();
        out.route.ttl = r.at("ttl").get<int>();

        const auto& d = j.at("delivery");
        out.delivery.qos = d.at("qos").get<int>();
        out.delivery.dup = d.at("dup").get<bool>();
        out.delivery.retain = d.at("retain").get<bool>();

        const auto& p = j.at("payload");
        copy_cstr(out.payload.building_id, p.at("building_id").get<std::string>());
        copy_cstr(out.payload.camera_id, p.at("camera_id").get<std::string>());
        copy_cstr(out.payload.description, p.at("description").get<std::string>());

        return true;
    }
    catch (...) {
        return false;
    }
}
