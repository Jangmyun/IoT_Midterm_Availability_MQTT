# test/core — core_broker 동작 검증

## 테스트 대상

**`core_broker` 바이너리** (`broker/src/core/main.cpp`)

## 목적

core_broker가 MQTT 브로커에 연결한 후 올바른 동작을 수행하는지 자동으로 검증합니다.
mosquitto_sub로 실제 발행된 메시지를 수신해 pass/fail을 판단합니다.

## 테스트 케이스

| ID | 스크립트 | 검증 내용 |
|---|---|---|
| CORE-01 | `01_bootstrap.sh` | core 연결 → ConnectionTable retain publish → edge 등록 수신 후 CT 갱신 broadcast |
| CORE-02 | `02_lwt.sh` | core 강제 종료(SIGKILL) → `campus/will/core/<id>` LWT에 backup core IP:port 포함 여부 |

## 테스트 흐름

```
CORE-01
  mosquitto_sub (campus/monitor/topology) 시작
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
```

## 실행

```bash
BUILD_DIR=./broker/build ./test/test_pub.sh core_bootstrap
BUILD_DIR=./broker/build ./test/test_pub.sh core_lwt

# 전체
BUILD_DIR=./broker/build ./test/test_pub.sh all_core
```

## 사전 조건

- `core_broker` 바이너리 빌드 완료 (`broker/build/core_broker`)
- mosquitto 브로커 실행 중
