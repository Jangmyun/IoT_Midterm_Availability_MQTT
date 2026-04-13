# Broker ↔ Client 연동 가이드

> 브로커 팀을 위한 문서.  
> C++ broker가 publish해야 할 토픽, JSON 포맷, 테스트 방법을 정리한다.

---

## 1. 전체 구조

```
C++ core_broker / edge_broker
        │  TCP 1883
        ▼
   mosquitto (MQTT server)
        │  WebSocket 9001
        ▼
React 모니터링 클라이언트
```

- **mosquitto** 는 독립 실행 서버. C++ 코드와 React 클라이언트 모두 mosquitto에만 연결한다.
- C++ broker 와 React client 사이에는 **직접 의존성이 없다.**
- C++ broker는 `libmosquitto` 로 `localhost:1883` 에 연결 후 publish.
- React client는 `mqtt.js` 로 `ws://localhost:9001` 에 연결 후 subscribe.

---

## 2. 로컬 테스트 환경 구성

### 2-1. mosquitto 실행 (WebSocket 활성화)

프로젝트 루트에 `mosquitto.conf` 가 이미 있다.

```bash
# 프로젝트 루트에서
mosquitto -c mosquitto.conf
```

기본 mosquitto는 WebSocket을 꺼두기 때문에 반드시 이 conf 파일로 실행해야 한다.  
conf 내용:

```
listener 1883
protocol mqtt

listener 9001
protocol websockets

allow_anonymous true
```

### 2-2. React 클라이언트 실행

```bash
cd client
npm install   # 첫 실행 시만
npm run dev
# → http://localhost:5173
```

### 2-3. 빠른 동작 확인 (test_pub.sh)

mosquitto_pub 으로 테스트 메시지를 쏴서 UI가 정상인지 먼저 확인할 수 있다.

```bash
# 프로젝트 루트에서
./test_pub.sh topology          # Connection Table → 그래프 + 브로커 카드 렌더링
./test_pub.sh event_intrusion   # HIGH 이벤트 → 이벤트 로그
./test_pub.sh node_down         # Node OFFLINE 알림 배너
./test_pub.sh core_switch       # Core 교체 → 재연결 배너
./test_pub.sh all               # 위 시나리오 순서대로 전체 실행
```

---

## 3. C++ broker가 publish해야 할 토픽 목록

| ID | 토픽 | 방향 | QoS | Retain | 설명 |
|----|------|------|-----|--------|------|
| M-04 | `campus/monitor/topology` | Core → Client | 1 | **Yes** | Connection Table 브로드캐스트 |
| D-01 | `campus/data/<event_type>` | Node → Core → Client | 1 | No | CCTV 이벤트 |
| D-02 | `campus/data/<event_type>/<building_id>` | Node → Core → Client | 1 | No | 건물별 세분화 이벤트 |
| A-01 | `campus/alert/node_down/<node_id>` | Core → Client | 1 | No | Node OFFLINE 알림 |
| A-02 | `campus/alert/node_up/<node_id>` | Core → Client | 1 | No | Node 복구 알림 |
| A-03 | `campus/alert/core_switch` | Core → Client | 1 | No | Active Core 교체 알림 → 클라이언트 재연결 유도 |
| W-01 | `campus/will/core/<core_id>` | mosquitto LWT | 1 | No | Core 비정상 종료 알림 → 클라이언트 재연결 유도 |

> **M-04는 Retain 필수.** 클라이언트가 늦게 접속해도 최신 CT를 즉시 받아야 하기 때문이다.

---

## 4. JSON 포맷

### 4-1. Connection Table (M-04)

토픽: `campus/monitor/topology`

```json
{
  "version": 3,
  "last_update": "2026-04-13T12:00:00",
  "active_core_id": "<uuid>",
  "backup_core_id": "<uuid>",
  "node_count": 4,
  "nodes": [
    {
      "id":          "<uuid>",
      "role":        "CORE",
      "ip":          "127.0.0.1",
      "port":        1883,
      "status":      "ONLINE",
      "hop_to_core": 0
    },
    {
      "id":          "<uuid>",
      "role":        "NODE",
      "ip":          "10.0.0.3",
      "port":        1883,
      "status":      "ONLINE",
      "hop_to_core": 2
    }
  ],
  "link_count": 1,
  "links": [
    {
      "from_id": "<uuid>",
      "to_id":   "<uuid>",
      "rtt_ms":  4.7
    }
  ]
}
```

**주의 사항:**
- `version` 은 CT가 갱신될 때마다 반드시 증가해야 한다. 클라이언트는 이전 버전 이하의 CT를 무시한다 (오래된 retained message 방지).
- `role` 값: `"CORE"` / `"NODE"` (대문자 고정)
- `status` 값: `"ONLINE"` / `"OFFLINE"` (대문자 고정)
- `nodes`, `links` 는 배열이어야 한다 (`node_count`, `link_count` 필드는 있어도 없어도 무방).

---

### 4-2. MqttMessage (D-01, D-02, A-01, A-02, A-03, W-01 공통)

```json
{
  "msg_id":    "<uuid>",
  "type":      "INTRUSION",
  "timestamp": "2026-04-13T12:01:00",
  "priority":  "HIGH",
  "source": {
    "role": "NODE",
    "id":   "<uuid>"
  },
  "target": {
    "role": "CORE",
    "id":   "<uuid>"
  },
  "route": {
    "original_node": "<uuid>",
    "prev_hop":      "<uuid>",
    "next_hop":      "<uuid>",
    "hop_count":     1,
    "ttl":           5
  },
  "delivery": {
    "qos":    1,
    "dup":    false,
    "retain": false
  },
  "payload": {
    "building_id": "bldg-a",
    "camera_id":   "cam-01",
    "description": "침입 감지 — 정문 CCTV"
  }
}
```

**필드별 주의 사항:**

| 필드 | 필수 | 값 |
|------|------|-----|
| `msg_id` | Yes | UUID 문자열. 클라이언트가 중복 수신 시 무시하므로 메시지마다 고유해야 함 |
| `type` | Yes | `MOTION`, `DOOR_FORCED`, `INTRUSION`, `STATUS`, `RELAY`, `LWT_CORE`, `LWT_NODE` 중 하나 |
| `priority` | No | `"HIGH"` / `"LOW"` / `"NORMAL"` — 없으면 필드 자체를 생략 (클라이언트가 `null` 처리) |
| `payload.building_id` | No | 이벤트 필터링에 사용. 없으면 필터에서 `?` 로 표시됨 |

**각 토픽별 권장 `type` 값:**

| 토픽 | 권장 type |
|------|-----------|
| `campus/data/INTRUSION[/…]` | `"INTRUSION"` |
| `campus/data/MOTION[/…]` | `"MOTION"` |
| `campus/data/DOOR_FORCED[/…]` | `"DOOR_FORCED"` |
| `campus/alert/node_down/…` | `"STATUS"` |
| `campus/alert/node_up/…` | `"STATUS"` |
| `campus/alert/core_switch` | `"STATUS"` |
| `campus/will/core/…` | `"LWT_CORE"` |

---

## 5. C++ 연동 시 주의 사항

### LWT(W-01) 등록 방법

Core broker는 mosquitto 연결 시 LWT를 미리 등록해야 한다.  
`mosquitto_connect_with_will()` 또는 `mosquitto_will_set()` 사용:

```c
// libmosquitto 예시
mosquitto_will_set(
    mosq,
    "campus/will/core/<core_id>",   // topic
    strlen(lwt_payload),            // payloadlen
    lwt_payload,                    // payload (MqttMessage JSON 문자열)
    1,                              // qos
    false                           // retain
);
```

`lwt_payload` 는 4-2의 MqttMessage JSON 을 직렬화한 문자열 (type = `"LWT_CORE"`).

### Core 교체 후 A-03 발행 순서

1. Backup Core가 새 Active Core로 전환
2. 새 CT(version 증가, `active_core_id` 갱신) 를 `campus/monitor/topology` 에 **retain** publish
3. `campus/alert/core_switch` 에 A-03 publish

클라이언트는 A-03 수신 시 현재 CT의 `backup_core_id` 노드로 재연결을 시도한다.  
**CT 갱신 → A-03 발행 순서를 지켜야** 재연결 후 올바른 topology를 볼 수 있다.

---

## 6. 수신 확인 (mosquitto_sub)

C++ broker가 publish한 메시지가 mosquitto에 잘 도달하는지 확인:

```bash
# 모든 토픽 감청 (와일드카드)
mosquitto_sub -h localhost -p 1883 -t "#" -v

# topology만
mosquitto_sub -h localhost -p 1883 -t "campus/monitor/topology"

# 이벤트만
mosquitto_sub -h localhost -p 1883 -t "campus/data/#" -v

# 알림 + LWT
mosquitto_sub -h localhost -p 1883 -t "campus/alert/#" -t "campus/will/#" -v
```

---

## 7. 전체 스택 실행 순서

```
① mosquitto -c mosquitto.conf          # 터미널 1
② cd client && npm run dev             # 터미널 2 → http://localhost:5173
③ ./build/core_broker &                # 터미널 3 (빌드 후)
④ ./build/edge_broker &                # 터미널 3
```

브로커 빌드:

```bash
cd broker
cmake -S . -B build
cmake --build build
```
