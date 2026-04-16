#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Parse "ip:port" string (stored in MqttMessage::payload.description for edge registration)
// Returns false if format is invalid or port is out of range
inline bool parse_ip_port(const char* desc, char* ip_out, size_t ip_len, int* port_out) {
    char ip_buf[64] = {};
    int  port = 0;
    if (sscanf(desc, "%63[^:]:%d", ip_buf, &port) != 2) return false;
    if (port <= 0 || port > 65535) return false;
    snprintf(ip_out, ip_len, "%s", ip_buf);
    *port_out = port;
    return true;
}
