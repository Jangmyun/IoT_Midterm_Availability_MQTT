# test/core — core_broker 동작 검증

## 테스트 대상

**`core_broker` 바이너리** (`broker/src/core/main.cpp`)

## 목적

core_broker가 MQTT 브로커에 연결한 후 올바른 동작을 수행하는지 자동으로 검증합니다.
mosquitto_sub로 실제 발행된 메시지를 수신해 pass/fail을 판단합니다.

## 테스트 케이스

| ID | 스크립트 | 검증 내용 | 관련 FR / 시나리오 |
|---|---|---|---|
| CORE-01 | `01_bootstrap.sh` | core 연결 → CT retain publish → edge 등록 수신 후 CT 갱신 broadcast | FR-01, FR-09, 시나리오 5.1/5.3 |
| CORE-02 | `02_lwt.sh` | core SIGKILL → `campus/will/core/<id>` LWT에 backup core IP:port 포함 | FR-04, W-01, 시나리오 5.4 |
| CORE-03 | `03_node_offline.sh` | Node LWT 수신 → OFFLINE 마킹 → `campus/alert/node_down` 발행 + CT OFFLINE 반영 | FR-06, A-01, 시나리오 5.5 |
| CORE-04 | `04_event_dedup.sh` | 동일 msg_id 이벤트 2회 발행 → core가 정확히 1회만 재발행하는지 확인 | FR-02, FR-03, 시나리오 5.1 |
| CORE-05 | `05_ct_sync.sh` | Active Core가 `_core/sync/connection_table` retained 발행 → edge 등록 후 버전 갱신 | FR-01, C-01, 시나리오 5.2 |
| CORE-06 | `06_core_switch.sh` | Active SIGKILL → Backup이 LWT 수신 → `campus/alert/core_switch` 발행 | FR-05, FR-14, A-03, 시나리오 5.4 |

## 테스트 흐름

```
CORE-01
  mosquitto_sub (campus/monitor/topology, campus/monitor/status/#) 시작
  → core_broker 실행
  → [core] connected 로그 확인
  → edge_broker 실행 (등록 트리거)
  → [edge] registered 로그 확인
  → topology 토픽에 edge 포함 CT 수신 확인

CORE-02
  mosquitto_sub (campus/will/core/#) 시작
  → core_broker 실행
  → [core] connected 로그 확인
  → SIGKILL
  → LWT 토픽 수신 확인
  → 페이로드에 backup_ip:backup_port 포함 확인

CORE-03
  mosquitto_sub (campus/monitor/topology, campus/alert/node_down/#) 시작
  → core_broker 실행
  → campus/monitor/status/<node_id> 발행 (M-03 노드 등록)
  → topology에 node 포함 확인
  → campus/will/node/<node_id> 발행 (LWT 시뮬레이션)
  → campus/alert/node_down/<node_id> 수신 확인
  → payload에 OFFLINE 상태 포함 확인

CORE-04
  mosquitto_sub (campus/data/#) 시작
  → core_broker 실행
  → 동일 msg_id로 campus/data/MOTION 2회 발행
  → core 로그에서 forward 횟수 = 1 확인

CORE-05
  mosquitto_sub (_core/sync/connection_table, campus/monitor/topology) 시작
  → core_broker Active 모드(argc=3)로 실행
  → [core] connected (ACTIVE) 로그 확인
  → _core/sync/connection_table retained 발행 확인
  → M-03 edge 등록 후 CT version >= 2 재발행 확인

CORE-06
  mosquitto_sub (campus/will/core/#, campus/alert/core_switch) 시작
  → Active core_broker 실행 (argc=3)
  → Backup core_broker 실행 (argc=5, peer→동일 broker)
  → [core/backup] connected to active broker 로그 확인
  → Active SIGKILL → LWT 수신 확인
  → campus/alert/core_switch 발행 확인
  → payload에 Backup IP 포함 확인
```

## 실행

```bash
# 개별 실행
BUILD_DIR=./broker/build ./test/core/03_node_offline.sh
BUILD_DIR=./broker/build ./test/core/04_event_dedup.sh
BUILD_DIR=./broker/build ./test/core/05_ct_sync.sh
BUILD_DIR=./broker/build ./test/core/06_core_switch.sh

# test_pub.sh 사용
BUILD_DIR=./broker/build ./test/test_pub.sh core_node_offline
BUILD_DIR=./broker/build ./test/test_pub.sh core_event_dedup
BUILD_DIR=./broker/build ./test/test_pub.sh core_ct_sync
BUILD_DIR=./broker/build ./test/test_pub.sh core_core_switch

# 전체 core 테스트
BUILD_DIR=./broker/build ./test/test_pub.sh all_core
```

## 사전 조건

- `core_broker` / `edge_broker` 바이너리 빌드 완료 (`broker/build/`)
- mosquitto 브로커 실행 중 (기본 포트 1883)
- `mosquitto_pub`, `mosquitto_sub` CLI 도구 설치
