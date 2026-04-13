# IoT Midterm Project - 2조

> 21900103 김민혁 / 22000771 추인규 / 22100061 권혁민

---

## 목차

1. [문제 정의](#문제-정의)
2. [문제 해결 방안](#문제-해결-방안)
3. [가정 상황](#가정-상황)
4. [MQTT 브로커 구조](#mqtt-브로커-구조)
5. [상황별 해결 시나리오](#상황별-해결-시나리오)
   - [정상적인 상황](#시나리오-1-정상적인-상황)
   - [Broker 연결 상태 변경 (Connection Table 업데이트)](#시나리오-2-broker-연결-상태-변경-connection-table-업데이트)
   - [신규 Broker 연결](#시나리오-3-신규-broker-연결)
   - [Core 브로커 중단 상황](#시나리오-4-core-브로커-중단-상황)
   - [노드 중단 상황 (1) - Connection Table 업데이트](#시나리오-5-노드-중단-상황-1---connection-table-업데이트)
   - [노드 중단 상황 (2) - 재전송 메커니즘](#시나리오-6-노드-중단-상황-2---재전송-메커니즘)
   - [초기 연결 및 Relay 노드 선택](#시나리오-7-초기-연결-및-relay-노드-선택)
6. [설계](#설계)
   - [MQTT 토픽](#mqtt-토픽-설계)
   - [MQTT 메시지 Payload](#mqtt-메시지-payload)
   - [Connection Table](#connection-table)
7. [웹페이지 및 클라이언트](#웹페이지-및-클라이언트)
8. [개발 우선순위](#개발-우선순위)

---

## 문제 정의

- **네트워크 불안정**: 각 건물의 Publisher가 CCTV 영상에서 발견·발생한 이벤트 로그를 전송하지만, 정상적 수신이 실패하는 상황 발생
- **MQTT 중앙 브로커 장애**: Core 브로커에 장애가 발생하는 경우, 이벤트 송수신 자체가 불가능한 상황 발생
- **트래픽 집중**: 트래픽이 특정 브로커에게 집중되는 경우, 이벤트 송·수신 장애 발생
- **이벤트 중복 기록**: 각 이벤트 간 구분이 모호한 상황 발생

---

## 문제 해결 방안

### 네트워크 불안정 → 이벤트 로그 수신 장애

- 이벤트가 발생한 Node는 해당 이벤트를 곧바로 Core에게 전송하지 않고, **인접한 Node에게 전송**
- 인접한 노드와의 **RTT**와 Core까지의 **Hop 거리** 등을 함께 고려하여 선정
- 이벤트는 inter-connected된 Node들을 따라 Core에게 최종적으로 전달

### Core MQTT 브로커 중단 → 이벤트 수신 불가

- **메인 Core와 대체 Core를 함께 동작**
- Core 비정상 종료 시 대체 Core의 IP를 **Last-Will Message**로 전송

### 트래픽 집중 → 송수신 장애

- 클라이언트와 연결되는 Core가 모든 이벤트 수신을 처리하지 않도록 **Core를 다수로 운영**
- Core들은 서로 **inter-connected** 상태 유지
- **Ping을 통해 RTT가 가장 짧고 대역폭을 가장 적게 사용하는 Node를 Core로 선정**하여 모든 참여 Node의 메시지 처리 형평성 보장

### 이벤트 중복 기록

- **UUID**를 통해 이벤트 식별
- **QoS Level 1**을 통한 at-least-once 전달 보장
- 전송 실패 시 해당 Node의 로컬 영역에 우선 메시지 저장 후, 재연결 시 재전송하거나 Core 브로커가 주기적으로 업데이트하는 **Connection Table을 참조하여 대체 Core에게 전송하는 Store and Forward 방식** 채택

---

## 가정 상황

- 각 건물의 브로커(Node 및 Edge)가 해당 건물의 이벤트를 수집하고 중앙 브로커(Core)로 전송
- Core 브로커들은 **동시 다발적으로 장애 상황이 발생하지 않음**
- 전체 네트워크의 모든 Core 및 Node들의 **UUID/IP/Port 정보는 변경되지 않음**

---

## MQTT 브로커 구조

```
Client / Monitoring
        │
        ▼
┌─────────────────────────────┐
│      CORE BROKER LAYER      │
│   Core A (Active) ◄──► Core B (Backup)  │
└─────────────────────────────┘
        │  Active connection (실선)
        │  Backup connection (점선)
        ▼
┌─────────────────────────────────────────────────┐
│                EDGE BROKER LAYER                │
│  Building A ◄··► Building B ◄··► Building C ◄··► Building D  │
│    (Edge)          (Edge)          (Edge)          (Edge)     │
└─────────────────────────────────────────────────┘

→  Active connection
--→ Backup connection
←→ Peer sync
```

- **Core Broker Layer**: Active Core + Backup Core로 구성, 서로 상태 동기화
- **Edge Broker Layer**: 각 건물별 Edge 브로커, 인접 Edge 브로커와 Peer sync
- Active Core는 모든 Edge에 Active connection 유지
- Backup Core는 모든 Edge에 Backup connection 유지

---

## 상황별 해결 시나리오

### 시나리오 1: 정상적인 상황

**1. Core 초기화**
- Active Core가 실행되며 Connection Table을 관리
- Core 간에는 동일한 Connection Table 정보가 동기화

**2. Node 연결**
- 각 건물의 Node Broker는 시작 시 Active Core에 우선 연결
- Node는 Connection Table을 수신한 뒤, 인접 Node 및 Core 경로를 결정

**3. Event 전달**
- Publisher가 생성한 이벤트는 Node Broker를 통해 Active Core로 전달
- Active Core는 UUID로 중복 여부를 확인한 뒤 Client에 이벤트 로그를 전달
- QoS 1을 통해 전달 신뢰성 보장

**흐름 요약:**
```
Publisher(CCTV)
    → [이벤트 생성]
    → Node Broker: 이벤트 전송 (msg_id, payload, route)
    → Active Core: msg_id 중복 확인
    → Active Core → Node Broker: ACK (QoS 1)
    → Active Core → Client: 이벤트 로그 전달
```

---

### 시나리오 2: Broker 연결 상태 변경 (Connection Table 업데이트)

**1. Broker 상태 변경 감지**
- Node Broker의 연결, 종료, 복구와 같은 상태 변화 발생
- 해당 상태 변화는 현재 Active Core에 전달되거나, Active Core가 이를 감지

**2. Connection Table 반영**
- Active Core는 변경된 Broker의 상태를 반영
- 이후 최신 상태를 기반으로 Connection Table을 업데이트

**3. 갱신 정보 전파**
- Active Core는 갱신된 Connection Table을 다른 Broker들에게 **브로드캐스트**
- 각 Broker는 수신한 최신 Connection Table을 바탕으로 네트워크 상태를 동기화

**흐름 요약:**
```
Node Broker → Active Core: 연결 상태 변경 (연결/종료/복구)
Active Core: Broker 상태 반영
Active Core: Connection Table 업데이트
Active Core --→ Other Brokers: 갱신된 Connection Table 브로드캐스트
```

---

### 시나리오 3: 신규 Broker 연결

**1. 신규 Broker 연결**
- 새 Broker가 Active Core에 초기 연결 요청
- Active Core는 해당 Broker를 네트워크에 등록

**2. Connection Table 업데이트**
- Active Core는 새 Broker 정보를 반영하여 Connection Table을 갱신
- 전체 네트워크의 최신 연결 상태 유지

**3. 최신 정보 전파**
- 새 Broker에게는 **retained message**로 최신 Connection Table 전달
- 다른 Broker들에게는 갱신된 Connection Table을 **브로드캐스트**

**4. 경로 초기화**
- 새 Broker는 전달받은 정보를 바탕으로 인접 Broker 및 relay 경로를 초기화

**흐름 요약:**
```
New Broker → Active Core: 초기 연결 요청
Active Core: New Broker 등록
Active Core: Connection Table 업데이트
Active Core → New Broker: Retained Message로 최신 Connection Table 전달
Active Core --→ Other Brokers: 갱신된 Connection Table 브로드캐스트
New Broker: 인접 Broker / relay 경로 초기화
```

---

### 시나리오 4: Core 브로커 중단 상황

**1. 장애 감지 및 알림**
- Main Core의 비정상 종료 발생
- **Last-Will Message 발행** 및 전체 Node 전파
- Payload에 대체 Core의 IP/Port 정보 포함

**2. 대체 Core 전환**
- Backup Core가 새로운 **Active Core로 전환**
- 기존 Connection Table과 이벤트 데이터 유지

**3. 재연결 및 서비스 지속**
- Node와 Client는 새 Core 정보로 재연결
- 저장된 이벤트를 **재전송**하여 서비스 지속

**4. 복구 후 동기화**
- 복구된 Main Core는 최신 상태를 동기화
- 이후 Backup Core(대체 Core)로 역할 전환

**흐름 요약:**
```
[정상 상태] Main Core ◄──► Backup Core: Connection Table / 이벤트 상태 동기화
Node Broker → Main Core: 이벤트 전송
Main Core: 비정상 종료 발생
Main Core: 등록된 LWT 발행
Main Core --→ Node Broker: 장애 알림 + Backup Core 정보
Main Core --→ Client: 장애 알림 + 새 Core 정보
Node Broker → Backup Core: 재연결
Client → Backup Core: 재연결 및 재구독
Node Broker → Backup Core: 저장된 이벤트 재전송
Backup Core: 새 Active Core로 동작 시작
```

---

### 시나리오 5: 노드 중단 상황 (1) - Connection Table 업데이트

**1. 노드 중단**
- Core가 Node의 **LWT(Last-Will Testament) 수신** 또는 Timeout으로 장애 감지
- Core가 Client에 해당 Node 중단 알림

**2. Connection Table 업데이트**
- Core가 Connection Table에서 해당 node의 상태를 **OFFLINE으로 업데이트**

**3. 최신 정보 전파**
- 갱신된 Connection Table을 다른 node들에 **Propagation**

**4. Publisher 동작**
- Publisher는 Connection Table을 참조하여 **인접 Node로 이벤트 전송(릴레이)**

**흐름 요약:**
```
Local Node: 비정상 종료
Active Core: Will Message 또는 Timeout으로 장애 감지
Active Core: Connection Table에서 N1 상태를 OFFLINE으로 변경
Active Core --→ Neighbor Node / Client: 갱신된 Connection Table 브로드캐스트 + N1 장애 알림
Publisher: 인접 Node로 이벤트 릴레이
Neighbor Node → Active Core: 이벤트 전달 (route 갱신)
Active Core: msg_id 중복 확인
Active Core → Client: 이벤트 로그 전달
```

---

### 시나리오 6: 노드 중단 상황 (2) - 재전송 메커니즘

**1. 노드 중단**
- 노드의 중단에 따른 이벤트 전송 실패
- 전송이 실패한 이벤트를 해당 **노드의 큐에 임시 저장(FIFO)** 후 정상적 연결 복구 대기

**2. 노드의 정상적인 복구**
- Core와의 **재연결 성공** 이후 큐에서 전송 실패했던 이벤트에 대해 **Dequeue**(큐에서 삭제되지는 않음) 수행
- 이후 해당 이벤트를 **재전송**

**3. Core 브로커의 이벤트 수신**
- 수신한 이벤트의 id를 통해 **중복 수신 여부 확인**
- Client에게 이벤트 로그를 전달 이후, 정상적인 수신에 대한 **ACK를 노드에게 전송**
- ACK를 수신한 노드는 Dequeue를 수행했던 메시지를 비로소 **큐에서 삭제**

**흐름 요약:**
```
Publisher → Node Broker: 이벤트 생성
Node Broker → Active Core: 이벤트 전송 실패 (×)
Node Broker → Local Queue: 이벤트 저장 (FIFO)
[연결 복구 대기]
Node Broker → Active Core: 재연결
Local Queue → Node Broker: 저장된 이벤트 dequeue (큐에서 삭제 X)
Node Broker → Active Core: 이벤트 재전송
Active Core: msg_id 중복 확인
Active Core → Client: 이벤트 로그 전달
Active Core → Node Broker: ACK
Node Broker → Local Queue: ACK 수신 후 큐에서 삭제
```

---

### 시나리오 7: 초기 연결 및 Relay 노드 선택

**1. 초기 연결**
- 새로운 Node가 Active Core에 초기 연결 요청
- Active Core는 최신 Connection Table을 전달

**2. 인접 Node 탐색**
- 새로운 Node는 Connection Table을 바탕으로 인접 Node 후보를 확인
- 이후 각 후보 Node와 **RTT를 측정**하여 응답 시간 비교

**3. Relay 노드 선택**
- **RTT가 가장 짧은 Node를 우선적으로 선택**
- RTT가 동일한 경우에는 **`hop_to_core` 값이 더 작은 Node를 선택**

**4. 경로 초기화**
- 선택된 Node를 기준으로 relay 경로 설정
- 이후 이벤트는 해당 경로를 따라 Active Core로 전달

**흐름 요약:**
```
New Node → Active Core: 초기 연결 요청
Active Core → New Node: Connection Table 전달
New Node → Neighbor Node A: RTT 측정 (ping)
New Node → Neighbor Node B: RTT 측정 (ping)
Neighbor Node A → New Node: RTT 응답 (pong)
Neighbor Node B → New Node: RTT 응답 (pong)
New Node: RTT가 가장 짧은 후보 선택
New Node: RTT 동일 시 hop_to_core가 더 작은 노드 선택
New Node: 릴레이 경로 설정
```

---

## 설계

### MQTT 토픽 설계

| No. | 카테고리 | 토픽 패턴 | 방향 | Publisher | Subscriber | QoS | 설명 |
|-----|----------|-----------|------|-----------|------------|-----|------|
| C-01 | Core 제어 | `_core/sync/connection_table` | Core → Core | Active Core | Backup Core(s) | 1 | Connection Table 동기화 |
| C-02 | Core 제어 | `_core/sync/load_info` | Core → Core | 각 Core | 다른 Core(s) | 1 | Core 부하 정보(대역폭, 메시지 수신량) 공유 |
| C-03 | Core 제어 | `_core/election/request` | Core → Core | Backup Core | 다른 Core(s) | 1 | 새 Active Core 선출 요청 |
| C-04 | Core 제어 | `_core/election/result` | Core → Core | 선출된 Core | 다른 Core(s) | 1 | 선출 결과 통보 |
| D-01 | 이벤트 데이터 | `campus/data/<event_type>` | Node → Core | Node | Core / Client | 1 | 건물별 CCTV 이벤트 로그 전달 |
| D-02 | 이벤트 데이터 | `campus/data/<event_type>/<building_id>` | Node → Core | Node | Core / Client | 1 | 건물별 세분화된 이벤트 전달 |
| R-01 | Node Relay | `campus/relay/<target_node_id>` | Node → Node | 송신 Node | 인접 Node | 1 | Core 직접 전달 불가 시 인접 노드 경유 릴레이 |
| R-02 | Node Relay | `campus/relay/ack/<msg_id>` | Node → Node | 수신 Node | 송신 Node | 1 | 릴레이 메시지 수신 확인 (중복 방지) |
| M-01 | 모니터링 | `campus/monitor/ping/<node_id>` | Node ↔ Node | Node | 인접 Node | 0 | RTT 측정용 Ping 요청 |
| M-02 | 모니터링 | `campus/monitor/pong/<node_id>` | Node ↔ Node | 대상 Node | 요청 Node | 0 | RTT 측정 Ping 응답 |
| M-03 | 모니터링 | `campus/monitor/status/<node_id>` | Node → Core | Node | Core | 1 | Node 상태 리포트 (주기적) |
| M-04 | 모니터링 | `campus/monitor/topology` | Core → Node | Core | 모든 Node | 1 | Connection Table 브로드캐스팅 |
| W-01 | Will Message | `campus/will/core/<core_id>` | Broker | 중단된 Core | 모든 Node/Client | 1 | Core 비정상 종료 시 대체 Core IP/Port 전달 |
| W-02 | Will Message | `campus/will/node/<node_id>` | Broker | 중단된 Node | Core | 1 | Node 비정상 종료 알림 |
| A-01 | Client 알림 | `campus/alert/node_down/<node_id>` | Core → Client | Core | Client | 1 | Node 비정상 종료 알림 (Client 향) |
| A-02 | Client 알림 | `campus/alert/node_up/<node_id>` | Core → Client | Core | Client | 1 | Node 복구 완료 알림 (Client 향) |
| A-03 | Client 알림 | `campus/alert/core_switch` | Core → Client | 새 Active Core | Client | 1 | Active Core 변경 알림 → Client 재연결 유도 |

---

### MQTT 메시지 Payload

#### 최상위 Payload 구조

| 필드 이름 | 타입 | 설명 |
|-----------|------|------|
| `msg_id` | UUID | 메시지 고유 식별자 |
| `type` | String | 메시지 종류 (실제 이벤트, 릴레이 이벤트 등) |
| `timestamp` | String | 메시지 생성 시간 |
| `source` | Object | 송신자 정보 |
| `target` | Object | 수신 대상 정보 |
| `priority` | String | 메시지 우선 순위 |
| `route` | Object | 메시지 전달 경로 정보 |
| `delivery` | Object | MQTT 전달 관련 정보 |
| `payload` | Object | 실제 데이터 내용 |

#### source / target 필드

| 필드 이름 | 타입 | 설명 |
|-----------|------|------|
| `source.role` | String | 송신자 역할 (Node / Core) |
| `source.id` | UUID | 송신자의 고유 ID |
| `target.role` | String | 수신 대상의 역할 |
| `target.id` | UUID | 수신 대상의 고유 ID |

#### route 필드

| 필드 이름 | 타입 | 설명 |
|-----------|------|------|
| `route.original_node` | UUID | 최초로 메시지를 생성한 Node ID |
| `route.prev_hop` | UUID | 직전에 메시지를 전송한 Node ID |
| `route.next_hop` | UUID | 다음으로 전송할 Node ID |
| `route.hop_count` | Integer | 현재까지 거쳐온 Hop의 수 |
| `route.ttl` | Integer | 메시지가 전달될 수 있는 최대 Hop의 수 |

#### delivery 필드

| 필드 이름 | 타입 | 설명 |
|-----------|------|------|
| `delivery.qos` | Int | MQTT QoS Level |
| `delivery.dup` | Bool | 중복 전송 여부 |
| `delivery.retain` | Bool | Retain Message 여부 |

---

### Connection Table

#### Connection Table 구조

| 필드 이름 | 타입 | 설명 |
|-----------|------|------|
| `version` | Int | Connection Table 버전 |
| `last_update` | Char[TIMESTAMP_LEN] | 마지막 업데이트 시간 (디버깅 및 상태 확인용) |
| `active_core_id` | Char[UUID_LEN] | 현재 Active Core의 UUID |
| `backup_core_id` | Char[UUID_LEN] | 장애 발생 시 대체할 Core의 UUID |
| `nodes` | NodeEntry[MAX_NODES] | 전체 Node/Core 목록 |
| `node_count` | Int | 현재 등록된 Node/Core 수 |
| `links` | LinkEntry[MAX_LINKS] | 전체 링크 정보 목록 |
| `link_count` | Int | 현재 등록된 링크 개수 |

#### Node Entry 구조

| 필드 이름 | 타입 | 설명 |
|-----------|------|------|
| `id` | char[UUID_LEN] | Node 또는 Core를 식별하는 UUID |
| `role` | NodeRole | 해당 엔트리가 Node 혹은 Core인지 구분 |
| `ip` | char[IP_LEN] | 해당 노드의 IPv4 주소 |
| `port` | uint16_t | MQTT 통신 포트 번호 |
| `status` | NodeStatus | 현재 상태 (ONLINE / OFFLINE) |
| `hop_to_core` | int | 해당 노드에서 Active Core까지의 hop 수 |

#### Link Entry 구조

| 필드 이름 | 타입 | 설명 |
|-----------|------|------|
| `from_id` | char[UUID_LEN] | 출발 노드 UUID |
| `to_id` | char[UUID_LEN] | 도착 노드 UUID |
| `rtt_ms` | float | 두 노드 간 RTT (ms) |

---

## 웹페이지 및 클라이언트

- **MQTT.js**를 통해 웹페이지 상에서 Core Broker와 연결
- **Graph Visualization 라이브러리**를 사용해 Connection Table 상에서 변경되는 노드 간 연결 상태를 실시간으로 확인할 수 있도록 제작

### 웹 클라이언트 주요 기능

- 온라인/오프라인 브로커 수 표시
- 연결된 브로커 수 표시
- 글로벌 Critical 이벤트 수 표시
- 총 이벤트 수신 수 표시
- **브로커 토폴로지 그래프** (ONLINE / WARNING / OFFLINE 상태 시각화)
  - Core 연결, 백업 연결, 노드 상태 실시간 반영
- **브로커 상태 카드** (Core / Edge별 상세 정보)
  - 상태, 역할, 연결 파라미터 수, 부하 %, 백업 경로 정보 등
- **실시간 이벤트 로그** 패널

---

## 개발 우선순위

### 개발 구현 우선도

1. Connection Table
2. Message Format
3. Node 최초 진입 시 Connection Table 참조
4. 모니터링 Client 측에서 데이터 수신 테스트
5. Backup Core 가동 및 Core 대체 과정 테스트

### 커버해야 할 시나리오 우선도

1. 정상 상황에서 정상 동작
2. Core 중단 시 대체 기능
3. Node init 시 RTT와 Hop 고려하여 초기화
4. Node 중단 시 로컬 큐잉 후 릴레이
5. 트래픽 집중 상황 대응
6. 클라이언트 (웹 페이지)
