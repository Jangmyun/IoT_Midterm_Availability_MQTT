# 실전 테스트 가이드

이 문서는 IoT MQTT 브로커 시스템을 실제 환경에서 검증하는 방법을 다룬다.
자동화 테스트(쉘 스크립트, pytest)와 수동 검증 절차를 모두 포함한다.

---

## 목차

1. [사전 준비](#1-사전-준비)
2. [바이너리 빌드](#2-바이너리-빌드)
3. [공통 환경 변수](#3-공통-환경-변수)
4. [쉘 스크립트 테스트 (`test_pub.sh`)](#4-쉘-스크립트-테스트)
5. [Python 통합 테스트 (pytest)](#5-python-통합-테스트)
6. [수동 시나리오 테스트](#6-수동-시나리오-테스트)
7. [분산 환경 테스트 (다중 호스트)](#7-분산-환경-테스트)
8. [트러블슈팅](#8-트러블슈팅)

---

## 1. 사전 준비

### 필수 도구

| 도구                              | 용도                     | 설치                                               |
| --------------------------------- | ------------------------ | -------------------------------------------------- |
| `mosquitto`                       | MQTT 브로커              | `brew install mosquitto` / `apt install mosquitto` |
| `mosquitto_pub` / `mosquitto_sub` | CLI 퍼블리셔·구독자      | mosquitto 패키지에 포함                            |
| `cmake` ≥ 3.20                    | C/C++ 빌드               | `brew install cmake` / `apt install cmake`         |
| `python3` ≥ 3.10                  | pytest 실행              | 시스템 또는 pyenv                                  |
| `paho-mqtt` 2.x                   | Python MQTT 클라이언트   | `pip install paho-mqtt`                            |
| `pytest` ≥ 7                      | Python 테스트 프레임워크 | `pip install pytest`                               |

### mosquitto 기동 확인

```bash
# 기동 (macOS)
brew services start mosquitto
# 또는 직접 실행
mosquitto -d

# 포트 확인
lsof -i :1883
```

> **주의**: 인증 없이 127.0.0.1:1883 에 익명 접속이 허용돼야 한다.
> `/etc/mosquitto/mosquitto.conf` 또는 `~/.mosquitto.conf` 에 아래를 추가:
>
> ```
> allow_anonymous true
> listener 1883 127.0.0.1
> ```

---

## 2. 바이너리 빌드

```bash
# 프로젝트 루트에서
cd broker
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

# 빌드 결과 확인
ls broker/build/core_broker broker/build/edge_broker
```

빌드 완료 후 기본 경로: `broker/build/core_broker`, `broker/build/edge_broker`

---

## 3. 공통 환경 변수

| 변수            | 기본값                   | 설명                                   |
| --------------- | ------------------------ | -------------------------------------- |
| `MQTT_HOST`     | `127.0.0.1`              | mosquitto 브로커 IP                    |
| `MQTT_PORT`     | `1883`                   | mosquitto 브로커 포트                  |
| `MQTT_USERNAME` | (없음)                   | 브로커 인증 사용자명                   |
| `MQTT_PASSWORD` | (없음)                   | 브로커 인증 비밀번호                   |
| `BUILD_DIR`     | `broker/build`           | 바이너리 디렉토리                      |
| `CORE_BINARY`   | `$BUILD_DIR/core_broker` | Core 바이너리 절대 경로 (오버라이드용) |
| `EDGE_BINARY`   | `$BUILD_DIR/edge_broker` | Edge 바이너리 절대 경로 (오버라이드용) |
| `CORE_A_IP`     | `127.0.0.1`              | Core A IP (client smoke용)             |
| `CORE_B_IP`     | `127.0.0.2`              | Core B IP (client smoke용)             |
| `NODE_1_IP`     | `10.0.0.3`               | Node 1 IP (client smoke용)             |

---

## 4. 쉘 스크립트 테스트

### 개요

`test/test_pub.sh` 는 세 가지 테스트 범주를 제공한다:

| 범주                | 대상                      | 전제 조건                                |
| ------------------- | ------------------------- | ---------------------------------------- |
| `client/smoke`      | 웹 클라이언트 렌더링      | mosquitto만 필요                         |
| `client/edge-cases` | 클라이언트 파싱·방어 로직 | mosquitto만 필요                         |
| `core/`             | core_broker 동작          | 빌드된 `core_broker` 필요                |
| `edge/`             | edge_broker 동작          | 빌드된 `core_broker`, `edge_broker` 필요 |

### 사용법

```bash
# 사용 가능한 케이스 목록 조회
./test/test_pub.sh list

# 개별 케이스 실행
./test/test_pub.sh core_bootstrap
./test/test_pub.sh edge_ping_pong

# 범주 전체 실행
./test/test_pub.sh all_client   # client 전체
./test/test_pub.sh all_core     # core 전체 (바이너리 없으면 자동 SKIP)
./test/test_pub.sh all_edge     # edge 전체

# 전체 실행
./test/test_pub.sh all
```

### 케이스별 설명

#### Client/Smoke

웹 클라이언트(`client/`)가 올바르게 렌더링하는지 검증. Core/Edge 바이너리 불필요.

```bash
./test/test_pub.sh topology          # topology JSON 발행 → 클라이언트 렌더링 확인
./test/test_pub.sh event_intrusion   # INTRUSION 이벤트 발행
./test/test_pub.sh node_down         # 노드 다운 알림
./test/test_pub.sh core_switch       # Core 전환 알림
./test/test_pub.sh lwt               # Core LWT
```

> 실행 후 웹 클라이언트(`http://localhost:3000` 등)에서 시각적으로 확인.

#### Core 시나리오

```bash
./test/test_pub.sh core_bootstrap       # Core 기동 + Edge 등록 → topology 발행
./test/test_pub.sh core_event_dedup     # 동일 msg_id 2회 → 1회만 재발행
./test/test_pub.sh core_node_offline    # Node LWT → node_down 알림
./test/test_pub.sh core_node_recovery   # Node 재등록 → node_up 알림
./test/test_pub.sh core_ct_sync         # 노드 등록 후 CT_SYNC 재발행
./test/test_pub.sh core_core_switch     # Active SIGKILL → Backup이 core_switch 발행
./test/test_pub.sh core_election        # 단일 Core Election
./test/test_pub.sh core_election_distributed  # 분산 Election (Active/Backup 간)
```

#### Edge 시나리오

```bash
./test/test_pub.sh edge_ping_pong             # Core → Edge ping/pong 왕복
./test/test_pub.sh edge_lwt                   # Edge LWT → Core가 node_down 발행
./test/test_pub.sh edge_core_switch           # core_switch 수신 → Edge 재연결 시도
./test/test_pub.sh edge_ct_active_core_change # CT active_core_id 변경 → Edge 재연결
```

### 테스트 로그 위치

각 스크립트는 `/tmp/mqtt-test.<name>.XXXXXX/` 아래에 로그를 남긴다.
테스트 종료 시 자동으로 정리된다. 실패 시 로그를 직접 확인하려면:

```bash
# 실패한 스크립트를 직접 수정해 cleanup trap 제거 후 재실행
# 또는 스크립트 내 show_file_tail 출력을 stderr에서 확인
./test/test_pub.sh core_bootstrap 2>&1 | tee /tmp/test_output.log
```

---

## 5. Python 통합 테스트

### 환경 설정

```bash
pip install paho-mqtt pytest

# 버전 확인
python3 -c "import paho.mqtt; print(paho.mqtt.__version__)"  # 2.x 이상
```

### 실행

```bash
# 프로젝트 루트에서
python3 -m pytest test/integration/ -v

# 개별 파일
python3 -m pytest test/integration/test_01_e2e.py -v
python3 -m pytest test/integration/test_02_failover.py -v

# 빌드 디렉토리가 기본값과 다를 때
BUILD_DIR=/path/to/build python3 -m pytest test/integration/ -v

# 로그 출력 포함
python3 -m pytest test/integration/ -v -s
```

### 테스트 파일 구성

| 파일                        | 시나리오           | 검증 항목                                          |
| --------------------------- | ------------------ | -------------------------------------------------- |
| `test_01_e2e.py`            | 이벤트 종단간 전달 | Publisher → Core 재발행 → Spy 수신, 필드 보존      |
| `test_02_failover.py`       | Core 페일오버      | Active SIGKILL → core_switch 발행, Edge 재연결     |
| `test_03_node_lifecycle.py` | Node 장애·복구     | LWT → node_down(OFFLINE), 재등록 → node_up(ONLINE) |
| `test_04_dedup.py`          | 이벤트 중복 방지   | 동일 msg_id 2회 → 1회만 재발행                     |
| `test_05_ct_sync.py`        | CT 동기화          | 노드 등록 후 CT 갱신·발행, Backup 수신             |

### 전제 조건

pytest 통합 테스트 실행 전:

1. mosquitto가 `127.0.0.1:1883` 에서 기동 중이어야 한다
2. `broker/build/core_broker`, `broker/build/edge_broker` 가 빌드돼 있어야 한다
3. (`BUILD_DIR` 환경 변수로 바이너리 경로 오버라이드 가능)

---

## 6. 수동 시나리오 테스트

자동화 테스트로 확인하기 어려운 상황을 직접 조작해 검증한다.

### 6.1 Core + Edge 기동 및 등록 확인

**터미널 A** — 구독자 (모니터):

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -v \
  -t "campus/monitor/topology" \
  -t "campus/monitor/status/#" \
  -t "campus/alert/#"
```

**터미널 B** — Active Core 기동:

```bash
./broker/build/core_broker 127.0.0.1 1883
```

**터미널 C** — Edge 기동:

```bash
./broker/build/edge_broker 127.0.0.1 1883 127.0.0.1 1883
```

**확인 사항**:

- 터미널 B 로그: `[core] <uuid> (ACTIVE) running`
- 터미널 C 로그: `[edge] registered: <uuid>`
- 터미널 A: `campus/monitor/topology` 메시지 수신, `campus/monitor/status/<edge-id>` 수신

---

### 6.2 Core 페일오버 (Active → Backup 전환)

**터미널 A** — 구독자:

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -v \
  -t "campus/will/core/#" \
  -t "campus/alert/core_switch"
```

**터미널 B** — Active Core:

```bash
./broker/build/core_broker 127.0.0.1 1883
# 기동 후 PID 기록
```

**터미널 C** — Backup Core:

```bash
./broker/build/core_broker 127.0.0.1 1883 127.0.0.1 1883
```

**터미널 D** — Edge:

```bash
./broker/build/edge_broker 127.0.0.1 1883 127.0.0.1 1883
```

**페일오버 트리거**:

```bash
# Active Core 강제 종료 (SIGKILL — LWT 트리거)
kill -9 <active_core_pid>
```

**확인 사항**:

- 터미널 A: `campus/will/core/<active-id>` 수신 (LWT)
- 터미널 A: `campus/alert/core_switch` 수신, payload.description에 Backup IP:Port 포함
- 터미널 C (Backup) 로그: `[core] ... (ACTIVE) running` (역할 전환)
- 터미널 D (Edge) 로그: `core_switch: reconnecting to <backup_ip>:<backup_port>`

---

### 6.3 Node 장애 감지 및 복구

**터미널 A** — 구독자:

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -v \
  -t "campus/alert/node_down/#" \
  -t "campus/alert/node_up/#" \
  -t "campus/monitor/topology"
```

**터미널 B** — Core 기동 후 Node 등록:

```bash
./broker/build/core_broker 127.0.0.1 1883

# 별도 터미널에서 Node 등록 (STATUS 메시지)
NODE_ID="cccccccc-0000-0000-0000-000000000003"
mosquitto_pub -h 127.0.0.1 -p 1883 \
  -t "campus/monitor/status/$NODE_ID" -q 1 \
  -m '{
    "msg_id":"'$(uuidgen | tr A-Z a-z)'",
    "type":"STATUS",
    "timestamp":"'$(date -u +%Y-%m-%dT%H:%M:%SZ)'",
    "source":{"role":"NODE","id":"'$NODE_ID'"},
    "target":{"role":"CORE","id":""},
    "route":{"original_node":"'$NODE_ID'","prev_hop":"'$NODE_ID'","next_hop":"","hop_count":0,"ttl":1},
    "delivery":{"qos":1,"dup":false,"retain":false},
    "payload":{"building_id":"","camera_id":"","description":"127.0.0.1:1883"}
  }'
```

**LWT 시뮬레이션** (노드 강제 종료 흉내):

```bash
mosquitto_pub -h 127.0.0.1 -p 1883 \
  -t "campus/will/node/$NODE_ID" -q 1 \
  -m '{
    "msg_id":"'$(uuidgen | tr A-Z a-z)'",
    "type":"STATUS",
    "timestamp":"'$(date -u +%Y-%m-%dT%H:%M:%SZ)'",
    "source":{"role":"NODE","id":"'$NODE_ID'"},
    "target":{"role":"CORE","id":""},
    "route":{"original_node":"'$NODE_ID'","prev_hop":"'$NODE_ID'","next_hop":"","hop_count":0,"ttl":1},
    "delivery":{"qos":1,"dup":false,"retain":false},
    "payload":{"building_id":"","camera_id":"","description":""}
  }'
```

**확인 사항**:

- `campus/alert/node_down/<NODE_ID>` 수신, payload에 `OFFLINE` 포함
- `campus/monitor/topology` 재발행 (Node 상태 갱신)

**복구 시뮬레이션** — 동일한 STATUS 메시지를 `campus/monitor/status/$NODE_ID` 에 재발행:

- `campus/alert/node_up/<NODE_ID>` 수신, payload에 `ONLINE` 포함

---

### 6.4 이벤트 중복 방지 확인

**구독자 + Core 기동 후**:

```bash
MSG_ID=$(uuidgen | tr A-Z a-z)
PAYLOAD='{
  "msg_id":"'$MSG_ID'",
  "type":"INTRUSION",
  "timestamp":"'$(date -u +%Y-%m-%dT%H:%M:%SZ)'",
  "priority":"HIGH",
  "source":{"role":"NODE","id":"'$(uuidgen | tr A-Z a-z)'"},
  "target":{"role":"CORE","id":"all"},
  "route":{"original_node":"x","prev_hop":"x","next_hop":"all","hop_count":1,"ttl":5},
  "delivery":{"qos":1,"dup":false,"retain":false},
  "payload":{"building_id":"bldg-a","camera_id":"cam-01","description":"test"}
}'

# 동일 msg_id로 2회 발행
mosquitto_pub -h 127.0.0.1 -p 1883 -t "campus/data/INTRUSION" -q 1 -m "$PAYLOAD"
sleep 1
mosquitto_pub -h 127.0.0.1 -p 1883 -t "campus/data/INTRUSION" -q 1 -m "$PAYLOAD"
```

**확인 사항**:

- Core 로그에서 `event forwarded: campus/data/INTRUSION` 가 정확히 **1회** 출력
- 구독자에서 `campus/data/INTRUSION` 메시지 수신 (원본 1회 + 재발행 1회 = 2회, 단 3회 이상이면 dedup 미작동)

---

### 6.5 CT 동기화 확인

```bash
# _core/sync/connection_table retained 메시지 감시
mosquitto_sub -h 127.0.0.1 -p 1883 -v \
  -t "_core/sync/connection_table" | python3 -m json.tool
```

Core + Edge 기동 후:

- 즉시 CT 수신 (version=1, edge 등록 전)
- Edge 등록 완료 후 CT 재수신 (version=2, nodes 배열에 edge 포함)

---

## 7. 분산 환경 테스트

실제 라즈베리파이 또는 다중 호스트에서 테스트할 때의 설정.

### 환경 예시

| 역할                    | 호스트          | 포트 |
| ----------------------- | --------------- | ---- |
| mosquitto (공유 브로커) | `192.168.1.100` | 1883 |
| Active Core             | `192.168.1.101` | —    |
| Backup Core             | `192.168.1.102` | —    |
| Edge                    | `192.168.1.103` | —    |

### 기동 순서

```bash
# 1. mosquitto (192.168.1.100)
mosquitto -d

# 2. Active Core (192.168.1.101)
./core_broker 192.168.1.100 1883

# 3. Backup Core (192.168.1.102)
#    마지막 두 인수 = Active Core의 mosquitto 주소
./core_broker 192.168.1.100 1883 192.168.1.100 1883

# 4. Edge (192.168.1.103)
#    인수: <자체 바인드 IP> <노드 포트> <Core broker IP> <Core broker 포트>
./edge_broker 192.168.1.103 1883 192.168.1.100 1883
```

### 분산 페일오버 검증

```bash
# 모니터링 (별도 머신)
MQTT_HOST=192.168.1.100 ./test/test_pub.sh core_core_switch

# 또는 직접 구독
mosquitto_sub -h 192.168.1.100 -p 1883 -v \
  -t "campus/will/core/#" \
  -t "campus/alert/core_switch" \
  -t "campus/monitor/topology"
```

### 환경 변수로 원격 실행

```bash
MQTT_HOST=192.168.1.100 \
MQTT_PORT=1883 \
CORE_BINARY=/path/to/core_broker \
EDGE_BINARY=/path/to/edge_broker \
./test/test_pub.sh all_core
```

---

## 8. 트러블슈팅

### mosquitto 연결 실패

```
Error: Connection refused
```

```bash
# 포트 확인
lsof -i :1883
# mosquitto 재기동
brew services restart mosquitto  # macOS
sudo systemctl restart mosquitto # Linux
```

### 바이너리를 찾을 수 없음 (`die "core_broker not found"`)

```bash
# 빌드 경로 확인
ls broker/build/core_broker

# 환경 변수로 명시
CORE_BINARY=$(pwd)/broker/build/core_broker ./test/test_pub.sh core_bootstrap
```

### Core가 ACTIVE로 뜨지 않음

Core 두 번째 인수(MQTT_PORT) 확인. mosquitto가 해당 포트에서 수신 중인지 확인:

```bash
./broker/build/core_broker 127.0.0.1 1883
# 로그에서 확인:
# [core] <uuid> (ACTIVE) running — OK
# [core] connection error: ... — 브로커 문제
```

### pytest 에서 `fixture 'active_core' not found`

```bash
# test/integration/ 디렉토리에서 실행하거나 경로를 명시
python3 -m pytest test/integration/ -v
```

### Backup Core가 peer 연결 안 됨

Backup Core 기동 인수의 4번째·5번째가 Active Core의 mosquitto 주소와 일치하는지 확인:

```bash
# OK: 같은 mosquitto를 가리킴
./core_broker 127.0.0.1 1883  127.0.0.1 1883
#             ^-- 자체 브로커  ^-- peer(Active) 브로커
```

### retained 메시지가 이전 테스트 결과를 오염시킴

```bash
# 수동으로 retained 메시지 초기화
mosquitto_pub -h 127.0.0.1 -p 1883 -t "campus/monitor/topology" -n -r
mosquitto_pub -h 127.0.0.1 -p 1883 -t "_core/sync/connection_table" -n -r
```

---

## 빠른 참조: 주요 토픽

| 토픽                          | 방향                     | 설명                                         |
| ----------------------------- | ------------------------ | -------------------------------------------- |
| `campus/data/#`               | Node→Core→Client         | CCTV 이벤트 (INTRUSION, MOTION, DOOR_FORCED) |
| `campus/monitor/topology`     | Core→All (retained)      | 전체 CT 스냅샷                               |
| `campus/monitor/status/<id>`  | Node→Core                | 노드 등록 / 상태 업데이트                    |
| `campus/monitor/ping/<id>`    | Core→Edge                | Ping 요청                                    |
| `campus/monitor/pong/<id>`    | Edge→Core                | Ping 응답                                    |
| `campus/alert/node_down/<id>` | Core→All                 | 노드 장애 알림 (OFFLINE)                     |
| `campus/alert/node_up/<id>`   | Core→All                 | 노드 복구 알림 (ONLINE)                      |
| `campus/alert/core_switch`    | Backup→All               | Core 전환 알림 (새 Active IP:Port)           |
| `campus/will/core/<id>`       | mosquitto LWT            | Core 비정상 종료 감지                        |
| `campus/will/node/<id>`       | mosquitto LWT            | Node 비정상 종료 감지                        |
| `_core/sync/connection_table` | Active→Backup (retained) | CT 동기화                                    |
| `_core/election/request`      | Core→Core                | Election 요청                                |
| `_core/election/result`       | Core→Core                | Election 결과(투표)                          |
