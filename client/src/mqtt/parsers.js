/**
 * parseConnectionTable
 *
 * campus/monitor/topology (M-04) 토픽에서 수신한 JSON 문자열을 파싱한다.
 * C++ connection_table_from_json() 과 필드 이름 1:1 대응.
 *
 * @param {string} raw - JSON 문자열
 * @returns {{ version, last_update, active_core_id, backup_core_id, nodes, links } | null}
 */
export function parseConnectionTable(raw) {
  try {
    const j = JSON.parse(raw);

    // C++ 파서와 동일한 필수 필드 확인
    if (typeof j.version !== 'number') return null;
    if (!Array.isArray(j.nodes) || !Array.isArray(j.links)) return null;

    return {
      version:        j.version,
      last_update:    j.last_update,
      active_core_id: j.active_core_id,
      backup_core_id: j.backup_core_id,
      nodes: j.nodes.map(n => ({
        id:          n.id,
        role:        n.role,        // "NODE" | "CORE"
        ip:          n.ip,
        port:        n.port,
        status:      n.status,      // "ONLINE" | "OFFLINE"
        hop_to_core: n.hop_to_core,
      })),
      links: j.links.map(l => ({
        from_id: l.from_id,
        to_id:   l.to_id,
        rtt_ms:  l.rtt_ms,
      })),
    };
  } catch {
    return null;
  }
}

/**
 * parseMqttMessage
 *
 * campus/data/#, campus/alert/#, campus/will/core/# 토픽에서 수신한 JSON 문자열을 파싱한다.
 * C++ mqtt_message_from_json() 과 필드 이름 1:1 대응.
 *
 * @param {string} raw - JSON 문자열
 * @returns {{ msg_id, type, timestamp, priority, source, target, route, delivery, payload } | null}
 */
export function parseMqttMessage(raw) {
  try {
    const j = JSON.parse(raw);

    // 필수 필드 확인 (C++ 파서 기준)
    if (!j.msg_id || !j.type) return null;

    return {
      msg_id:    j.msg_id,
      type:      j.type,       // "MOTION"|"DOOR_FORCED"|"INTRUSION"|"LWT_CORE"|"LWT_NODE"|"STATUS"|...
      timestamp: j.timestamp,
      priority:  j.priority ?? null,  // optional (C++: PRIORITY_NONE if omitted)
      source:    j.source,     // { role: "NODE"|"CORE", id: uuid }
      target:    j.target,     // { role: "NODE"|"CORE", id: uuid }
      route:     j.route,      // { original_node, prev_hop, next_hop, hop_count, ttl }
      delivery:  j.delivery,   // { qos, dup, retain }
      payload:   j.payload,    // { building_id, camera_id, description }
    };
  } catch {
    return null;
  }
}
