// message.js
// MqttMessage 생성 순수 함수 — MQTT 의존 없이 단위 테스트 가능
//
// 발행 토픽: campus/data/<type>/<building>/<camera>
// 페이로드: MqttMessage JSON (C++ broker와 동일한 포맷)

export const MSG_TYPES = {
  MOTION:     'MOTION',
  DOOR_FORCED:'DOOR_FORCED',
  INTRUSION:  'INTRUSION',
};

// MsgType → 토픽 세그먼트 (C++ msg_type_to_topic_segment 와 동일)
const TYPE_TO_SEGMENT = {
  MOTION:     'motion',
  DOOR_FORCED:'door',
  INTRUSION:  'intrusion',
};

// MsgType → priority (C++ infer_priority 와 동일)
const TYPE_TO_PRIORITY = {
  MOTION:     'MEDIUM',
  DOOR_FORCED:'HIGH',
  INTRUSION:  'HIGH',
};

// ISO 8601 UTC — 밀리초 없이 ("2026-04-18T12:00:00Z")
export function nowUtc() {
  return new Date().toISOString().slice(0, 19) + 'Z';
}

export function inferPriority(type) {
  return TYPE_TO_PRIORITY[type] ?? null;
}

// "campus/data/<segment>/<building>/<camera>"
// building 또는 camera 가 비어있으면 null 반환
export function buildTopic(type, buildingId, cameraId) {
  const segment = TYPE_TO_SEGMENT[type];
  if (!segment || !buildingId || !cameraId) return null;
  return `campus/data/${segment}/${buildingId}/${cameraId}`;
}

// MqttMessage 객체 생성
// publisherId, type, buildingId, cameraId 는 필수
export function buildMessage({ publisherId, type, buildingId, cameraId, description = '', qos = 1 }) {
  if (!publisherId) throw new Error('publisherId required');
  if (!MSG_TYPES[type]) throw new Error(`unknown type: ${type}`);

  return {
    msg_id:    crypto.randomUUID(),
    type,
    timestamp: nowUtc(),
    priority:  inferPriority(type),
    source:    { role: 'NODE', id: publisherId },
    target:    { role: 'CORE', id: '' },
    route: {
      original_node: publisherId,
      prev_hop:      '',
      next_hop:      '',
      hop_count:     0,
      ttl:           8,
    },
    delivery: { qos, dup: false, retain: false },
    payload: {
      building_id: buildingId,
      camera_id:   cameraId,
      description,
    },
  };
}
