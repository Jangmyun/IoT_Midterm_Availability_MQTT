#pragma once

#include <string>
#include "message.h"
#include "connection_table.h"

// Connection Table
std::string connection_table_to_json(const ConnectionTable& ct);
bool        connection_table_from_json(const std::string& json, ConnectionTable& out);

// MQTT Message
std::string mqtt_message_to_json(const MqttMessage& msg);
bool        mqtt_message_from_json(const std::string& json, MqttMessage& out);