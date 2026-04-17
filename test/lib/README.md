# test/lib — 공용 헬퍼

모든 테스트 스크립트에서 `source` 하는 공용 라이브러리입니다.

## 파일

| 파일 | 역할 |
|---|---|
| `common.sh` | MQTT publish/subscribe 래퍼, 바이너리 탐색, UUID 생성, 로그, 프로세스 관리 |
| `payloads.sh` | JSON 페이로드 빌더 (ConnectionTable, MqttMessage, Event, Ping 등) |

## common.sh 주요 함수

| 함수 | 설명 |
|---|---|
| `mqtt_publish_json <topic> <qos> <retain> <payload>` | mosquitto_pub 래퍼 |
| `start_subscriber <logfile> <topic...>` | mosquitto_sub를 백그라운드로 실행, PID 반환 |
| `start_core <logfile>` | core_broker 바이너리 실행, PID 반환 |
| `start_edge <logfile>` | edge_broker 바이너리 실행, PID 반환 |
| `wait_for_pattern <file> <regex> [timeout]` | 파일에서 패턴이 나타날 때까지 대기 |
| `kill_forcefully <pid>` | SIGKILL 후 wait |
| `extract_core_id <logfile>` | core 로그에서 UUID 추출 |
| `extract_edge_id <logfile>` | edge 로그에서 UUID 추출 |
| `gen_uuid` | UUID 생성 (uuidgen / /proc / python3 순으로 시도) |

## payloads.sh 주요 함수

| 함수 | 생성 포맷 | 용도 |
|---|---|---|
| `emit_topology_json <ver> [active] [backup] [n1_status] [n2_status]` | ConnectionTable | topology, node_down/up 알림 |
| `emit_event_json <type> <priority> <src> <dst> <bldg> <cam> <desc> [msg_id]` | MqttMessage | CCTV 이벤트 |
| `emit_status_json <src_role> <src_id> ...` | MqttMessage (STATUS) | edge 등록 |
| `emit_core_lwt_payload_json <src> <backup_id> <ip> <port>` | MqttMessage (LWT_CORE) | core LWT |
| `emit_ping_json <requester> <target>` | MqttMessage (PING_REQ) | RTT 측정 |

## 환경변수 기본값 (common.sh)

| 변수 | 기본값 |
|---|---|
| `MQTT_HOST` | `127.0.0.1` |
| `MQTT_PORT` | `1883` |
| `CORE_A_ID` | `aaaaaaaa-0000-0000-0000-000000000001` |
| `CORE_B_ID` | `bbbbbbbb-0000-0000-0000-000000000002` |
| `NODE_1_ID` | `cccccccc-0000-0000-0000-000000000003` |
| `NODE_2_ID` | `dddddddd-0000-0000-0000-000000000004` |
