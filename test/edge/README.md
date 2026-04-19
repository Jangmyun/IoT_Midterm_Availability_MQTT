# test/edge — edge_broker 동작 검증

## 테스트 대상

**`edge_broker` 바이너리** (`broker/src/edge/main.cpp`)

## 목적

edge_broker가 core에 연결·등록한 후 올바른 동작을 수행하는지 자동으로 검증합니다.
core_broker도 함께 실행하여 실제 연동 흐름을 검증합니다.

## 테스트 케이스

| ID | 스크립트 | 검증 내용 |
|---|---|---|
| EDGE-01 | `01_ping_pong.sh` | `campus/monitor/ping/<edge_id>` 발행 → edge가 `campus/monitor/pong/<requester_id>` 응답 |
| EDGE-02 | `02_lwt.sh` | edge 강제 종료(SIGKILL) → `campus/will/node/<edge_id>` LWT 발행 확인 |
| EDGE-03 | `03_core_switch.sh` | `campus/alert/core_switch` 수신 → edge가 새 Active Core로 재연결 시도 |
| EDGE-04 | `04_ct_active_core_change.sh` | topology의 `active_core_id` 변경 수신 → edge가 새 Core IP:Port로 재연결 |
| EDGE-05 | `05_rtt_relay.sh` | Edge 2대 등록 → Ping/Pong RTT 계산 → relay node 선택 + peer OFFLINE 전파 확인 |
| EDGE-06 | `06_store_and_forward.sh` | upstream broker 단절 중 로컬 이벤트 큐 저장 → 재연결 후 queued event flush |
| EDGE-08 | `08_failover_rejoin.sh` | Active Core failover 후 edge 재기동 → backup CT로 promoted active 재학습 → 이벤트 전달 유지 |

## 테스트 흐름

```
EDGE-01
  mosquitto_sub (campus/monitor/pong/#) 시작
  → core_broker 실행 → [core] connected 확인
  → edge_broker 실행 → [edge] registered 확인
  → PING_REQ publish (campus/monitor/ping/<edge_id>)
  → PONG 수신 확인 (campus/monitor/pong/<requester_id>)

EDGE-02
  mosquitto_sub (campus/will/node/#) 시작
  → core_broker 실행 → [core] connected 확인
  → edge_broker 실행 → [edge] registered 확인
  → SIGKILL (edge)
  → LWT 토픽 수신 확인 (campus/will/node/<edge_id>)
```

> **참고**: EDGE-02에서 LWT 발행은 검증되지만, core가 이를 수신해 `campus/alert/node_down/<id>`를
> publish하고 클라이언트가 topology를 갱신하는 전체 흐름은 클라이언트를 띄워서 브라우저로 확인하세요.

## 실행

```bash
BUILD_DIR=./broker/build ./test/test_pub.sh edge_ping_pong
BUILD_DIR=./broker/build ./test/test_pub.sh edge_lwt
BUILD_DIR=./broker/build ./test/test_pub.sh edge_rtt_relay
BUILD_DIR=./broker/build ./test/test_pub.sh edge_store_forward
BUILD_DIR=./broker/build ./test/test_pub.sh edge_failover_rejoin

# 전체
BUILD_DIR=./broker/build ./test/test_pub.sh all_edge
```

## 사전 조건

- `core_broker`, `edge_broker` 바이너리 빌드 완료
- mosquitto 브로커 실행 중
