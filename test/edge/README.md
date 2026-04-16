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

# 전체
BUILD_DIR=./broker/build ./test/test_pub.sh all_edge
```

## 사전 조건

- `core_broker`, `edge_broker` 바이너리 빌드 완료
- mosquitto 브로커 실행 중
