# 테스트 시나리오

`test/` 디렉토리는 **대상 컴포넌트 기준**으로 분류되어 있습니다.

## 디렉토리 구조

```
test/
  lib/                  공용 헬퍼 (UUID 생성, MQTT publish/subscribe, 페이로드 빌더)
  client/               웹 클라이언트 렌더링·파싱 검증 (mosquitto_pub만 필요)
    smoke/              mock 데이터 publish → 클라이언트 렌더링 확인 (PUB-01~08)
    edge-cases/         비정상 입력 → 클라이언트 방어 로직 확인 (EC-01~04)
  core/                 core_broker 동작 검증 (바이너리 필요)
  edge/                 edge_broker 동작 검증 (바이너리 필요)
  test_pub.sh           케이스 디스패처
```

## 빠른 시작

```bash
# 클라이언트 전체 (브로커만 있으면 바로 실행)
./test/test_pub.sh all_client

# core 테스트
BUILD_DIR=./broker/build ./test/test_pub.sh all_core

# edge 테스트
BUILD_DIR=./broker/build ./test/test_pub.sh all_edge

# 전부 한번에
BUILD_DIR=./broker/build ./test/test_pub.sh all
```

원격 브로커 또는 다른 IP 사용 시:

```bash
MQTT_HOST=192.168.0.40 MQTT_PORT=1883 \
CORE_A_IP=192.168.0.40 CORE_B_IP=192.168.0.41 \
NODE_1_IP=192.168.0.51 NODE_2_IP=192.168.0.52 \
./test/test_pub.sh all_client
```

브로커에 인증이 필요한 경우 `MQTT_USERNAME`과 `MQTT_PASSWORD`를 환경변수로 설정하세요.

---

## client/smoke — 렌더링 스모크 테스트

웹 클라이언트가 각 메시지 타입을 올바르게 렌더링하는지 확인합니다. `mosquitto_pub`만 있으면 실행 가능합니다.

| ID | 스크립트 | 관련 요구사항 | 검증 내용 | MQTT 토픽 |
| --- | --- | --- | --- | --- |
| PUB-01 | `client/smoke/01_topology.sh` | M-04, FR-11 | retained 토폴로지 페이로드 → 그래프/카드 렌더링 | `campus/monitor/topology` (retain=true) |
| PUB-02 | `client/smoke/02_event_intrusion.sh` | D-01 | HIGH 우선순위 침입 이벤트 렌더링 | `campus/data/INTRUSION` |
| PUB-03 | `client/smoke/03_event_motion.sh` | D-02 | 건물 범위 모션 이벤트 렌더링 | `campus/data/MOTION/bldg-b` |
| PUB-04 | `client/smoke/04_event_door_forced.sh` | D-02 | 강제 문 열림 이벤트 렌더링 | `campus/data/DOOR_FORCED/bldg-c` |
| PUB-05 | `client/smoke/05_node_down.sh` | A-01 | 노드 오프라인 알림 배너 | `campus/alert/node_down/<id>` |
| PUB-06 | `client/smoke/06_node_up.sh` | A-02 | 노드 복구 알림 배너 | `campus/alert/node_up/<id>` |
| PUB-07 | `client/smoke/07_core_switch.sh` | A-03, FR-14 | 클라이언트 페일오버 배너 트리거 | `campus/alert/core_switch` |
| PUB-08 | `client/smoke/08_core_lwt.sh` | W-01 | synthetic core-down LWT 알림 렌더링 | `campus/will/core/<id>` |

`90_all_smoke.sh`는 PUB-01~08을 순서대로 실행합니다. `99_clear_topology.sh`는 retained 토폴로지 토픽을 초기화합니다.

---

## client/edge-cases — 파싱·방어 로직 테스트

비정상 또는 경계 입력에 대해 클라이언트가 올바르게 처리하는지 확인합니다.

| ID | 스크립트 | 관련 요구사항 | 검증 내용 | 기대 동작 |
| --- | --- | --- | --- | --- |
| EC-01 | `client/edge-cases/01_duplicate_event.sh` | FR-02 | 동일 `msg_id` 이벤트 2회 publish | msg_id 기준 중복 제거, 1건만 표시 |
| EC-02 | `client/edge-cases/02_stale_topology.sh` | FR-09 | 토폴로지 v10 → v9 순서로 publish | 낮은 버전 무시 |
| EC-03 | `client/edge-cases/03_invalid_event_uuid.sh` | parser contract | `msg_id`가 UUID 형식이 아닌 이벤트 | 파서가 메시지 drop |
| EC-04 | `client/edge-cases/04_missing_priority.sh` | optional field | priority 필드 없는 이벤트 | 정상 수신, priority를 null로 처리 |

---

## core — core_broker 동작 검증

`core_broker` 바이너리가 필요합니다. `BUILD_DIR` 또는 `CORE_BINARY` 환경변수로 경로를 지정하세요.

| ID | 스크립트 | 관련 요구사항 | 검증 내용 |
| --- | --- | --- | --- |
| CORE-01 | `core/01_bootstrap.sh` | 5.1, 5.3 | core 연결 + 토폴로지 publish + edge 등록 확인 |
| CORE-02 | `core/02_lwt.sh` | 5.4, FR-04 | core 강제 종료 → LWT에 backup 엔드포인트 포함 여부 확인 |

```bash
BUILD_DIR=./broker/build ./test/test_pub.sh core_bootstrap
BUILD_DIR=./broker/build ./test/test_pub.sh core_lwt
```

---

## edge — edge_broker 동작 검증

`core_broker` + `edge_broker` 바이너리가 모두 필요합니다.

| ID | 스크립트 | 관련 요구사항 | 검증 내용 |
| --- | --- | --- | --- |
| EDGE-01 | `edge/01_ping_pong.sh` | 5.6, M-01, M-02 | PING_REQ publish → edge PONG 응답 수신 확인 |
| EDGE-02 | `edge/02_lwt.sh` | 5.5, W-02 | edge 강제 종료 → `campus/will/node/<id>` LWT 발행 확인 |

> **참고 (EDGE-02):** LWT 발행은 검증되지만, core 측 `campus/alert/node_down` publish는 미구현 상태입니다.

```bash
BUILD_DIR=./broker/build ./test/test_pub.sh edge_ping_pong
BUILD_DIR=./broker/build ./test/test_pub.sh edge_lwt
```

---

## 미자동화 케이스 (구현 대기)

`PRD.md` 14.2절 기재 기능. 해당 기능 구현 전에는 자동화 테스트 통과 불가.

- `FR-03`: `CCTV → Edge → Core → Client` 엔드-투-엔드 이벤트 전달
- `FR-05`: `W-01` 수신 후 backup core 페일오버
- `FR-06`: LWT 수신 후 core의 `campus/alert/node_down/<id>` publish
- `FR-07`: 재연결 후 store-and-forward 큐 플러시
- `FR-08`: RTT 측정 및 중계 노드 선택

---

## 환경 변수

| 변수 | 기본값 | 설명 |
| --- | --- | --- |
| `MQTT_HOST` | `127.0.0.1` | MQTT 브로커 호스트 |
| `MQTT_PORT` | `1883` | MQTT 브로커 포트 |
| `MQTT_USERNAME` | (없음) | 브로커 인증 사용자명 |
| `MQTT_PASSWORD` | (없음) | 브로커 인증 패스워드 |
| `CORE_A_IP`, `CORE_B_IP` | `127.0.0.1`, `127.0.0.2` | 토폴로지 페이로드에 삽입될 core IP |
| `NODE_1_IP`, `NODE_2_IP` | `10.0.0.3`, `10.0.0.4` | 토폴로지 페이로드에 삽입될 노드 IP |
| `BUILD_DIR` | `./build` | 통합 테스트용 바이너리 기본 탐색 경로 |
| `CORE_BINARY`, `EDGE_BINARY` | (없음) | 바이너리 명시 경로 (BUILD_DIR 대신 사용) |
| `EDGE_NODE_PORT` | `2883` | `edge_broker` 로컬 바인딩 포트 |
| `BACKUP_CORE_ID`, `BACKUP_CORE_IP`, `BACKUP_CORE_PORT` | (기본값) | core LWT 테스트용 backup core 정보 |

## 기타

- `./test/test_pub.sh list`로 사용 가능한 케이스 이름 목록을 확인할 수 있습니다.
- `core/`, `edge/` 테스트는 임시 실행 디렉토리를 생성하고 종료 시 자동으로 삭제합니다. 실패 시 관련 로그 마지막 부분을 출력합니다.
- `all_core` / `all_edge` / `all` 실행 시 바이너리가 없으면 에러 없이 해당 그룹을 건너뜁니다.
