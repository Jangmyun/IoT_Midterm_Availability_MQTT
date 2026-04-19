# Product Requirements Document

# 스마트 캠퍼스 CCTV 이벤트 전송 시스템

### MQTT 기반 고가용성 분산 브로커 설계

> **프로젝트:** IoT 중간 프로젝트 | 한동대학교 컴퓨터공학 | 2조  
> **팀원:** 김민혁 (21900103) · 추인규 (22000771) · 권혁민 (22100061)  
> **문서 버전:** v1.0 | **작성일:** 2025

---

## 목차

1. [문서 개요](#1-문서-개요)
2. [문제 정의](#2-문제-정의)
3. [해결 방안 요약](#3-해결-방안-요약)
4. [시스템 아키텍처](#4-시스템-아키텍처)
5. [상황별 시나리오 요구사항](#5-상황별-시나리오-요구사항)
6. [MQTT 토픽 설계](#6-mqtt-토픽-설계)
7. [MQTT 메시지 Payload 설계](#7-mqtt-메시지-payload-설계)
8. [Connection Table 설계](#8-connection-table-설계)
9. [기능 요구사항](#9-기능-요구사항)
10. [비기능 요구사항](#10-비기능-요구사항)
11. [개발 우선순위](#11-개발-우선순위)
12. [전제 조건 및 제약](#12-전제-조건-및-제약)
13. [부록](#13-부록)

---

## 1. 문서 개요

### 1.1 목적

본 PRD(Product Requirements Document)는 네트워크 불안정·브로커 장애·트래픽 집중·이벤트 중복 발생 상황에서도 스마트 캠퍼스 CCTV 이벤트 로그를 안정적으로 수집·전달하기 위한 **MQTT 기반 분산 브로커 시스템**의 기능적·비기능적 요구사항을 정의합니다.

### 1.2 범위

- MQTT Broker 계층 설계 (Core / Edge / Node)
- Connection Table 설계 및 동기화 메커니즘
- 장애 감지 및 자동 페일오버 시나리오
- Relay 경로 선택 알고리즘 (RTT + hop_to_core)
- 웹 기반 모니터링 클라이언트 (MQTT.js + Graph Visualization)

### 1.3 용어 정의

| 용어                  | 정의                                                                            |
| --------------------- | ------------------------------------------------------------------------------- |
| **Core Broker**       | 전체 이벤트를 집중 처리하는 중앙 MQTT 브로커. Active/Backup 이중화.             |
| **Edge Broker**       | 각 건물에 배치된 로컬 MQTT 브로커. Core의 부하를 분산하고 근거리 이벤트를 수집. |
| **Node Broker**       | Edge와 동의어로 사용. Publisher(CCTV)에서 가장 가까운 브로커.                   |
| **Connection Table**  | 전체 네트워크 토폴로지, 노드 상태, 링크 RTT를 담는 중앙 상태 데이터베이스.      |
| **LWT**               | Last-Will Testament. MQTT 클라이언트 비정상 종료 시 자동 발행되는 메시지.       |
| **RTT**               | Round-Trip Time. 두 노드 간 네트워크 왕복 지연 시간(ms).                        |
| **hop_to_core**       | 특정 Node에서 Active Core까지 거쳐야 하는 브로커 홉(hop) 수.                    |
| **Store-and-Forward** | 전송 실패 이벤트를 로컬 큐에 저장 후 연결 복구 시 재전송하는 방식.              |
| **QoS 1**             | MQTT at-least-once 전달 보장 레벨. ACK 기반 중복 방지.                          |
| **UUID**              | 이벤트·노드를 고유 식별하는 범용 고유 식별자(RFC 4122).                         |

---

## 2. 문제 정의

현행 중앙집중형 MQTT 구조에서 다음 네 가지 핵심 문제가 식별되었습니다.

| #        | 문제                                      | 영향                                       |
| -------- | ----------------------------------------- | ------------------------------------------ |
| **P-01** | 네트워크 불안정으로 인한 이벤트 수신 실패 | 이벤트 누락 → 보안 사각지대 발생           |
| **P-02** | 중앙 Core 브로커 단일 장애점(SPOF)        | 전체 이벤트 송수신 불가                    |
| **P-03** | 트래픽 특정 브로커 집중                   | 처리 지연 및 메시지 손실                   |
| **P-04** | 이벤트 중복 기록                          | 이벤트 간 구분 모호, 감사 로그 신뢰성 저하 |

---

## 3. 해결 방안 요약

### 3.1 네트워크 불안정 대응 (P-01)

- 이벤트 발생 Node는 Core 직접 전송 전 인접 Node를 경유하는 Relay 경로를 우선 시도
- RTT + hop_to_core 기준 최적 Relay Node 동적 선택
- 전송 실패 시 로컬 FIFO 큐에 저장 → 연결 복구 후 재전송 (Store-and-Forward)

### 3.2 Core SPOF 제거 (P-02)

- Active Core + Backup Core 이중화 운영
- Core 비정상 종료 시 LWT에 대체 Core IP/Port 포함 → 전 Node 자동 재연결
- Connection Table과 이벤트 데이터는 Backup Core에 실시간 동기화

### 3.3 트래픽 분산 (P-03)

- 다수의 Core를 inter-connected 상태로 운영하여 트래픽 분산
- Ping RTT 및 대역폭 기준으로 최적 Core를 동적 선출 (Election 메커니즘)
- Core 간 부하 정보(`_core/sync/load_info`) 주기적 공유

### 3.4 중복 이벤트 방지 (P-04)

- UUID 기반 `msg_id`로 이벤트 고유 식별
- QoS 1을 통한 at-least-once 전달 보장
- Core에서 `msg_id` 중복 확인 후 Client 전달

---

## 4. 시스템 아키텍처

### 4.1 브로커 계층 구조

```
                  Client / Monitoring
                          │
          ┌───────────────▼───────────────┐
          │         CORE BROKER LAYER      │
          │                               │
          │   Core A (Active) ◄──────► Core B (Backup)  │
          └───────────────────────────────┘
                ↙   ↙   ↘   ↘         (Active: 실선 / Backup: 점선)
          ┌─────────────────────────────────────────┐
          │           EDGE BROKER LAYER              │
          │                                         │
          │  [Bldg A] ◄──► [Bldg B] ◄──► [Bldg C] ◄──► [Bldg D]  │
          └─────────────────────────────────────────┘
               │          │          │          │
           CCTV(Pub)  CCTV(Pub)  CCTV(Pub)  CCTV(Pub)
```

| 계층       | 구성                          | 역할                                                               |
| ---------- | ----------------------------- | ------------------------------------------------------------------ |
| Core Layer | Active Core A + Backup Core B | Peer Sync로 Connection Table 및 이벤트 상태 동기화                 |
| Edge Layer | Building A~D Edge             | Active Core와 Active Connection, Core B와 Backup Connection 유지   |
| Publisher  | CCTV 카메라                   | 가장 가까운 Edge Broker에 이벤트 발행                              |
| Client     | 웹 모니터링                   | Core Broker에 WebSocket(MQTT)으로 연결, 토픽 구독 기반 실시간 수신 |

### 4.2 연결 종류

| 연결 유형         | 발신                 | 수신   | 설명                                            |
| ----------------- | -------------------- | ------ | ----------------------------------------------- |
| Active Connection | Edge                 | Core A | 정상 상태 주 연결 경로                          |
| Backup Connection | Edge                 | Core B | Core A 장애 시 자동 전환 대상                   |
| Peer Sync         | Core A ↔ Core B      | 상호   | Connection Table, 이벤트 상태, 부하 정보 동기화 |
| Relay             | Node → Neighbor Node | Core   | Core 직접 전달 불가 시 인접 노드 경유           |
| Peer (Edge)       | Edge ↔ Edge          | 상호   | 인접 건물 Edge 간 RTT 측정 및 릴레이 경로 공유  |

---

## 5. 상황별 시나리오 요구사항

### 5.1 정상 동작 시나리오

1. Active Core가 실행되어 Connection Table 초기화 및 관리 시작
2. 각 Edge Broker가 Active Core에 연결 → Connection Table 수신 → 인접 Edge 및 Core 경로 결정
3. Publisher(CCTV)가 이벤트 생성 → Edge Broker 경유 → Active Core 전달
4. Active Core는 `msg_id`(UUID) 중복 확인 후 Client에 이벤트 로그 전달 (QoS 1)

```
Publisher(CCTV)  Node Broker    Active Core      Client
      │               │               │             │
      │─ 이벤트 생성 →│               │             │
      │               │─ 이벤트 전송 →│             │
      │               │   (msg_id,    │             │
      │               │   payload,    │             │
      │               │   route)      │             │
      │               │               │─ msg_id ──→ │
      │               │               │  중복 확인  │
      │               │←─── ACK(QoS1)─│             │
      │               │               │── 이벤트 ──→│
      │               │               │   로그 전달 │
```

### 5.2 Broker 상태 변경 (Connection Table 업데이트)

1. Node Broker 연결/종료/복구 상태 변화 감지 (LWT 또는 직접 감지)
2. Active Core가 변경 사항을 Connection Table에 반영
3. 갱신된 Connection Table을 모든 Broker에게 브로드캐스트 (`M-04` 토픽)
4. 각 Broker는 최신 Connection Table 기반으로 네트워크 상태 동기화

### 5.3 신규 Broker 연결

1. 새 Broker → Active Core 초기 연결 요청
2. Active Core: Broker 등록 → Connection Table 갱신
3. 새 Broker에게 Retained Message로 최신 Connection Table 전달
4. 기존 Broker들에게 갱신된 Connection Table 브로드캐스트
5. 새 Broker: RTT 측정 → 인접 Broker 및 Relay 경로 초기화

### 5.4 Core 브로커 중단 시나리오

1. Main Core 비정상 종료 → LWT 발행 (Payload: 대체 Core IP/Port 포함)
2. LWT가 모든 Node/Client에 전파
3. Backup Core가 새 Active Core로 전환 (기존 Connection Table + 이벤트 데이터 유지)
4. Node/Client는 새 Core 정보로 재연결
5. 로컬 큐에 저장된 이벤트 재전송 (Store-and-Forward)
6. 복구된 Main Core: 최신 상태 동기화 후 Backup Core로 전환

```
Node Broker    Main Core    Backup Core      Client
     │             │             │              │
     │             │─ CT / 이벤트 상태 동기화 →│
     │─ 이벤트 전송→│             │              │
     │         비정상 종료        │              │
     │             │(LWT 발행)   │              │
     │←─── 장애 알림 + Backup Core 정보 ───────│
     │             │←─── 장애 알림 + 새 Core 정보
     │─────────────────── 재연결 ─────────────→│
     │                           │← 재연결 및 구독
     │─ 저장 이벤트 재전송 ──────→│              │
     │                           │── 이벤트 로그→│
     │                           │ (새 Active Core로 동작 시작)
```

### 5.5 Node 중단 시나리오

#### 5.5.1 Relay 경유 처리

1. Core가 Node의 LWT 수신 → Client에 Node 중단 알림
2. Connection Table에서 해당 Node 상태를 `OFFLINE`으로 업데이트
3. 갱신된 Connection Table 브로드캐스트
4. Publisher: Connection Table 참조 → **다른 ONLINE Edge 후보 중 RTT 최소, RTT 동점 시 `hop_to_core` 최소** 기준으로 이벤트 릴레이
5. 대체 Edge가 없으면 **Active Core 직접 연결**로 전환
6. 최신 CT 스냅샷이 `nodes=[]` 로 비어 있더라도, Publisher는 **마지막으로 수신한 Active Core endpoint를 캐시**해 두었다가 직접 연결 시도

#### 5.5.2 Store-and-Forward 처리

1. Publisher가 이벤트 생성 → Node Broker 전송 시도 실패
2. 이벤트를 로컬 FIFO 큐에 저장 후 연결 복구 대기
3. 연결 복구 후 저장 이벤트 dequeue → Active Core로 재전송
4. Active Core: `msg_id` 중복 확인 → Client에 전달 → ACK 수신 후 큐에서 삭제

### 5.6 초기 연결 및 Relay 노드 선택

1. 새 Node → Active Core 초기 연결 요청 → Connection Table 수신
2. Connection Table 기반 인접 Node 후보 탐색
3. 각 후보 Node와 RTT 측정 (`campus/monitor/ping`, `campus/monitor/pong` 토픽)
4. **RTT 최소 Node 선택; RTT 동점 시 `hop_to_core` 최소 Node 선택**
5. 선택된 Node를 기준으로 Relay 경로 설정 → 이후 이벤트는 해당 경로로 전달

---

## 6. MQTT 토픽 설계

| No.      | 카테고리    | 토픽 패턴                                | 방향        | QoS | 설명                                       |
| -------- | ----------- | ---------------------------------------- | ----------- | --- | ------------------------------------------ |
| **C-01** | Core 제어   | `_core/sync/connection_table`            | Core→Core   | 1   | Connection Table 동기화                    |
| **C-02** | Core 제어   | `_core/sync/load_info`                   | Core→Core   | 1   | 부하 정보(대역폭/수신량) 공유              |
| **C-03** | Core 제어   | `_core/election/request`                 | Core→Core   | 1   | 새 Active Core 선출 요청                   |
| **C-04** | Core 제어   | `_core/election/result`                  | Core→Core   | 1   | 선출 결과 통보                             |
| **D-01** | 이벤트      | `campus/data/<event_type>`               | Node→Core   | 1   | 건물별 CCTV 이벤트 로그 전달               |
| **D-02** | 이벤트      | `campus/data/<event_type>/<building_id>` | Node→Core   | 1   | 건물별 세분화 이벤트 전달                  |
| **R-01** | Node Relay  | `campus/relay/<target_node_id>`          | Node→Node   | 1   | Core 직접 전달 불가 시 릴레이              |
| **R-02** | Node Relay  | `campus/relay/ack/<msg_id>`              | Node→Node   | 1   | 릴레이 수신 확인 (중복 방지)               |
| **M-01** | 모니터링    | `campus/monitor/ping/<node_id>`          | Node↔Node   | 0   | RTT 측정용 Ping 요청                       |
| **M-02** | 모니터링    | `campus/monitor/pong/<node_id>`          | Node↔Node   | 0   | RTT 측정 응답                              |
| **M-03** | 모니터링    | `campus/monitor/status/<node_id>`        | Node→Core   | 1   | Node 상태 리포트 (주기적)                  |
| **M-04** | 모니터링    | `campus/monitor/topology`                | Core→Node   | 1   | Connection Table 브로드캐스트              |
| **W-01** | LWT         | `campus/will/core/<core_id>`             | Broker      | 1   | Core 비정상 종료 시 대체 Core IP/Port 전달 |
| **W-02** | LWT         | `campus/will/node/<node_id>`             | Broker      | 1   | Node 비정상 종료 알림                      |
| **A-01** | Client 알림 | `campus/alert/node_down/<node_id>`       | Core→Client | 1   | Node 비정상 종료 알림                      |
| **A-02** | Client 알림 | `campus/alert/node_up/<node_id>`         | Core→Client | 1   | Node 복구 완료 알림                        |
| **A-03** | Client 알림 | `campus/alert/core_switch`               | Core→Client | 1   | Active Core 변경 알림 → Client 재연결 유도 |

---

## 7. MQTT 메시지 Payload 설계

### 7.1 최상위 Payload 구조

| 필드명      | 타입            | 필수 | 설명                                                   |
| ----------- | --------------- | ---- | ------------------------------------------------------ |
| `msg_id`    | UUID (string)   | Y    | 메시지 고유 식별자 — 중복 필터링 기준                  |
| `type`      | string          | Y    | 메시지 종류 (MOTION, DOOR_FORCED, INTRUSION, RELAY 등) |
| `timestamp` | ISO 8601 string | Y    | 메시지 생성 UTC 시각                                   |
| `source`    | Object          | Y    | 송신자 정보 (role, id)                                 |
| `target`    | Object          | Y    | 수신 대상 정보 (role, id)                              |
| `priority`  | string          | N    | 우선순위 (HIGH / MEDIUM / LOW)                         |
| `route`     | Object          | Y    | 메시지 전달 경로 정보                                  |
| `delivery`  | Object          | Y    | MQTT 전달 설정 (qos, dup, retain)                      |
| `payload`   | Object          | Y    | 실제 이벤트 데이터                                     |

```jsonc
{
  "msg_id": "550e8400-e29b-41d4-a716-446655440000",
  "type": "INTRUSION",
  "timestamp": "2025-04-12T09:31:00Z",
  "priority": "HIGH",
  "source": { "role": "NODE", "id": "uuid-edge-a" },
  "target": { "role": "CORE", "id": "uuid-core-a" },
  "route": {
    "original_node": "uuid-edge-a",
    "prev_hop": "uuid-edge-a",
    "next_hop": "uuid-core-a",
    "hop_count": 1,
    "ttl": 5,
  },
  "delivery": { "qos": 1, "dup": false, "retain": false },
  "payload": {
    "building_id": "building-a",
    "camera_id": "cam-a-01",
    "description": "침입 의심 객체 감지",
  },
}
```

### 7.2 source / target 객체

| 필드명 | 타입          | 설명                              |
| ------ | ------------- | --------------------------------- |
| `role` | string        | `NODE` 또는 `CORE`                |
| `id`   | UUID (string) | 송신자 또는 수신 대상의 고유 UUID |

### 7.3 route 객체

| 필드명          | 타입    | 설명                                              |
| --------------- | ------- | ------------------------------------------------- |
| `original_node` | UUID    | 최초 메시지를 생성한 Node UUID                    |
| `prev_hop`      | UUID    | 직전에 메시지를 전송한 Node UUID                  |
| `next_hop`      | UUID    | 다음으로 전송할 Node UUID (없으면 Core)           |
| `hop_count`     | integer | 현재까지 거쳐온 Hop 수                            |
| `ttl`           | integer | 메시지가 전달 가능한 최대 Hop 수 (무한 루프 방지) |

### 7.4 delivery 객체

| 필드명   | 타입            | 설명                  |
| -------- | --------------- | --------------------- |
| `qos`    | integer (0/1/2) | MQTT QoS Level        |
| `dup`    | boolean         | 중복 전송 여부 플래그 |
| `retain` | boolean         | Retain Message 여부   |

---

## 8. Connection Table 설계

### 8.1 최상위 구조

| 필드명           | 타입                 | 설명                                              |
| ---------------- | -------------------- | ------------------------------------------------- |
| `version`        | integer              | Connection Table 버전 번호 — 구 버전 수신 시 무시 |
| `last_update`    | char[TIMESTAMP_LEN]  | 마지막 업데이트 시각 (ISO 8601)                   |
| `active_core_id` | char[UUID_LEN]       | 현재 Active Core의 UUID                           |
| `backup_core_id` | char[UUID_LEN]       | 장애 발생 시 대체할 Core UUID                     |
| `nodes`          | NodeEntry[MAX_NODES] | 전체 Node/Core 목록                               |
| `node_count`     | integer              | 현재 등록된 Node/Core 수                          |
| `links`          | LinkEntry[MAX_LINKS] | 전체 링크 정보 목록                               |
| `link_count`     | integer              | 현재 등록된 링크 수                               |

### 8.2 Node Entry

| 필드명        | 타입              | 설명                                   |
| ------------- | ----------------- | -------------------------------------- |
| `id`          | char[UUID_LEN]    | Node 또는 Core를 식별하는 UUID         |
| `role`        | NodeRole (enum)   | `NODE` 혹은 `CORE`                     |
| `ip`          | char[IP_LEN]      | 해당 노드의 IPv4 주소                  |
| `port`        | uint16_t          | MQTT 통신 포트 번호                    |
| `status`      | NodeStatus (enum) | 현재 상태 (`ONLINE` / `OFFLINE`)       |
| `hop_to_core` | integer           | 해당 노드에서 Active Core까지의 hop 수 |

### 8.3 Link Entry

| 필드명    | 타입           | 설명                       |
| --------- | -------------- | -------------------------- |
| `from_id` | char[UUID_LEN] | 출발 노드 UUID             |
| `to_id`   | char[UUID_LEN] | 도착 노드 UUID             |
| `rtt_ms`  | float          | 두 노드 간 측정된 RTT (ms) |

### 8.4 C 구조체 참고

```c
typedef enum { NODE_ROLE_NODE, NODE_ROLE_CORE } NodeRole;
typedef enum { NODE_STATUS_ONLINE, NODE_STATUS_OFFLINE } NodeStatus;

typedef struct {
    char       id[UUID_LEN];
    NodeRole   role;
    char       ip[IP_LEN];
    uint16_t   port;
    NodeStatus status;
    int        hop_to_core;
} NodeEntry;

typedef struct {
    char  from_id[UUID_LEN];
    char  to_id[UUID_LEN];
    float rtt_ms;
} LinkEntry;

typedef struct {
    int        version;
    char       last_update[TIMESTAMP_LEN];
    char       active_core_id[UUID_LEN];
    char       backup_core_id[UUID_LEN];
    NodeEntry  nodes[MAX_NODES];
    int        node_count;
    LinkEntry  links[MAX_LINKS];
    int        link_count;
} ConnectionTable;
```

---

## 9. 기능 요구사항

| ID        | 요구사항                                                                          | 우선순위 | 관련 시나리오       |
| --------- | --------------------------------------------------------------------------------- | -------- | ------------------- |
| **FR-01** | Connection Table을 JSON 직렬화하여 Core 간 동기화 (`_core/sync/connection_table`) | P0       | 정상, Core 전환     |
| **FR-02** | UUID 기반 `msg_id`로 수신 이벤트 중복 필터링                                      | P0       | 정상, Node 중단     |
| **FR-03** | QoS 1 적용으로 이벤트 at-least-once 전달 보장                                     | P0       | 전 시나리오         |
| **FR-04** | Core 비정상 종료 시 LWT에 대체 Core IP/Port 포함 발행                             | P0       | Core 중단           |
| **FR-05** | Backup Core가 LWT 수신 후 Active Core로 자동 전환                                 | P0       | Core 중단           |
| **FR-06** | Node 비정상 종료 시 LWT 발행 및 Client 알림 (`A-01` 토픽)                         | P0       | Node 중단           |
| **FR-07** | 전송 실패 이벤트를 로컬 FIFO 큐에 저장 후 재전송 (Store-and-Forward); C++ Edge 및 JS publisher_client 모두 적용 | P0 | Node 중단 |
| **FR-08** | RTT 및 `hop_to_core` 기반 최적 Relay Node 선택                                    | P1       | Relay 선택          |
| **FR-09** | 갱신된 Connection Table을 Retained Message로 신규 Broker에 전달                   | P1       | 신규 Broker 연결    |
| **FR-10** | Core 간 부하 정보 주기적 공유 및 RTT 기반 Active Core 선출                        | P1       | 트래픽 분산         |
| **FR-11** | MQTT.js 기반 웹 클라이언트에서 Core Broker WebSocket 연결 및 이벤트 구독          | P1       | 모니터링 클라이언트 |
| **FR-12** | Graph Visualization 라이브러리로 Connection Table 변경 상태 실시간 표시           | P1       | 모니터링 클라이언트 |
| **FR-13** | Node 복구 완료 시 `A-02` 토픽으로 Client에 복구 알림 전송                         | P2       | Node 중단           |
| **FR-14** | Active Core 변경 시 `A-03` 토픽으로 Client 재연결 유도                            | P2       | Core 중단           |
| **FR-15** | Edge_broker가 Core로부터 수신한 CT를 Local mosquitto에 retained로 재발행 — publisher_client가 Edge WebSocket에서 CT 수신 가능 | P1 | Publisher Failover |
| **FR-16** | JS publisher_client가 `campus/monitor/topology` 구독 후 **다른 ONLINE Edge 후보 중 RTT+hop_to_core 기준 최적 Edge**를 자동 선택하고, Edge 후보가 없으면 **Active Core 직접 연결**로 Failover 수행 | P1 | Publisher Failover |
| **FR-17** | publisher_client가 CT-OFFLINE 감지(사전) + WebSocket disconnect(사후) 두 경로로 Failover 트리거, **stale CT(version 감소/동일) 무시**, Primary Edge 복구 시 더 최신 CT 기준으로 자동 복귀 | P1 | Publisher Failover |
| **FR-19** | Publisher 계열(pub_sim, publisher_client)은 마지막으로 본 Active Core endpoint를 캐시하여, **CT가 비어 있는 스냅샷(`nodes=[]`)만 남아도 direct-core fallback** 을 수행 | P1 | Publisher Failover |
| **FR-18** | 모니터링 Client 이벤트 카드에 Publisher ID(`pub:xxxx`) 표시 — `payload.description` 내 중첩 JSON에서 추출 | P2 | 이벤트 식별 |

---

## 10. 비기능 요구사항

| ID         | 항목      | 요구사항                                                    |
| ---------- | --------- | ----------------------------------------------------------- |
| **NFR-01** | 가용성    | Core 장애 발생 후 60초 이내 Backup Core로 전환 완료         |
| **NFR-02** | 신뢰성    | 이벤트 전달 성공률 99% 이상 (QoS 1 + Store-and-Forward)     |
| **NFR-03** | 지연      | RTT 200ms 이하인 경로에서 이벤트 End-to-End 전달            |
| **NFR-04** | 확장성    | Node 최대 64개 이상 Connection Table에 등록 가능            |
| **NFR-05** | 동시성    | 동시 장애 Core는 1개로 제한 (설계 전제 조건)                |
| **NFR-06** | 보안      | IP/Port 정보는 LWT Payload에 한정 노출, MQTT 인증 적용 권장 |
| **NFR-07** | 운영 환경 | IP/Port는 배포 후 변경 불가 (정적 주소 가정)                |

---

## 11. 개발 우선순위

### 11.1 컴포넌트 개발 순서

1. Connection Table 설계 및 구현 (C 구조체 + JSON 직렬화)
2. Message Format 정의 및 파서 구현
3. Node 최초 진입 시 Connection Table 참조 및 경로 초기화
4. 모니터링 Client 측 데이터 수신 기본 테스트
5. Backup Core 가동 및 Core 대체 과정 End-to-End 테스트

### 11.2 시나리오 검증 우선순위

| 우선순위 | 시나리오                                | 검증 기준                                       |
| -------- | --------------------------------------- | ----------------------------------------------- |
| **P0**   | 정상 동작 — 이벤트 end-to-end 전달      | CCTV → Edge → Core → Client 이벤트 수신 확인    |
| **P0**   | Core 중단 시 대체 Core 자동 전환        | LWT 수신 후 60초 이내 재연결 완료               |
| **P1**   | Node 초기화 시 RTT + Hop 기반 경로 선택 | 최적 Relay Node 선택 결과 로그 확인             |
| **P1**   | Node 중단 시 로컬 큐잉 후 릴레이 전송   | Store-and-Forward로 이벤트 0 누락 검증          |
| **P2**   | 트래픽 집중 상황 부하 분산              | 다수 Publisher 동시 발행 시 Core 부하 균등 분배 |
| **P3**   | 웹 클라이언트 모니터링 UI               | Graph 실시간 업데이트 및 이벤트 로그 표시       |

---

## 12. 전제 조건 및 제약

- 각 건물의 Edge Broker가 해당 건물의 CCTV(Publisher) 이벤트를 1차 수집하고 Core로 전달한다.
- Core 브로커들은 동시 다발적으로 장애가 발생하지 않는다 (최대 1개 Core 장애 가정).
- 전체 네트워크의 모든 Core 및 Node의 UUID/IP/Port 정보는 배포 후 변경되지 않는다.
- Edge 간 Peer Sync는 RTT 측정 및 Relay 경로 공유 목적으로만 사용한다.
- QoS 2는 사용하지 않으며, 중복 처리는 Core의 `msg_id` 확인으로 대체한다.

---

## 14. 현재 개발 진행도 (2026-04-17 기준, Phase 3 완료)

### 14.1 구현 완료 항목

#### 공통 인프라

| 구성요소 | 파일 |
| -------- | ---- |
| ConnectionTable C 구조체 + 상수 (MAX_NODES=64, MAX_LINKS=256) | `broker/include/connection_table.h` |
| ConnectionTableManager — thread-safe CRUD (mutex, addNode, updateNode, setNodeStatus, addLink, snapshot) | `broker/src/common/connection_table_manager.cpp` |
| JSON 직렬화/역직렬화 양방향 (CT + MqttMessage, enum 변환 포함) | `broker/src/common/mqtt_json.cpp` |
| UUID 생성기 (RFC 4122 v4, thread-local mt19937_64) | `broker/include/uuid.h` |
| MqttMessage 구조체 + 토픽 상수 21개 정의 (TOPIC_NODE_REGISTER 포함) | `broker/include/message.h` |
| 순수 로직 헬퍼 — parse_ip_port, make_alert_topic, merge_connection_tables | `broker/include/core_helpers.h` |
| Edge 순수 로직 헬퍼 — infer_msg_type, infer_priority, parse_building_camera, select_relay_node | `broker/include/edge_helpers.h` |
| nlohmann/json 3.11.3 FetchContent, 크로스플랫폼 CMake 빌드 시스템 | `broker/CMakeLists.txt` |
| 직렬화/역직렬화 단위 테스트 (TC-01~TC-09, 9개 케이스) | `broker/test/test_json.cpp` |
| Core 로직 단위 테스트 (TC-01~TC-08, 8개 케이스) | `broker/test/test_core_logic.cpp` |
| Edge 로직 단위 테스트 (TC-01~TC-08, 32 assertions) | `broker/test/test_edge_logic.cpp` |

#### Core Broker (`broker/src/core/main.cpp`)

| 구성요소 | 관련 FR/시나리오 |
| -------- | --------------- |
| argc 기반 Active/Backup 역할 자동 인식 | — |
| Active: `<broker_host> <broker_port>` / Backup: 추가 `<active_core_ip> <active_core_port>` | — |
| 7개 토픽 구독 + CT Retained publish (TOPIC_TOPOLOGY) | M-04 |
| Edge 등록(M-03) 수신 → description 파싱 → CT addNode → 브로드캐스트 | 5.3 |
| Node LWT(W-02) 수신 → OFFLINE 마킹 → CT 브로드캐스트 → `campus/alert/node_down` 발행 | FR-06, W-02, A-01 |
| msg_id 중복 필터(unordered_set, cap=10000) + 이벤트 QoS 1 republish | FR-02, FR-03 |
| Active Core LWT(W-01) 설정 (자신 종료 알림) | W-01 |
| CT 양방향 동기화: TOPIC_CT_SYNC(Active→Backup, retained), TOPIC_NODE_REGISTER(Backup→Active) | FR-01, C-01 |
| Backup mosq_peer — Active 브로커 연결, merge_connection_tables() (변경 시만 재발행) | FR-01, C-01 |
| Backup: Active LWT 수신 → `campus/alert/core_switch` 발행 (자신 IP:Port 포함) | FR-05, FR-14, A-03 |
| CT 수신 시 version 비교 → 구버전 무시 (`on_message`, `on_message_peer`) | FR-01 |
| Node 복구(OFFLINE→ONLINE) 감지 → `campus/alert/node_up/<id>` 발행 | FR-13, A-02 |
| Core Election — `_core/election/request` 투표, `_core/election/result` 집계 → ACTIVE 전환 | FR-10, C-03, C-04 |
| Core Election 분산 환경 지원 — `on_connect_peer()`에서 `_core/election/#` 구독, peer 채널 투표·결과 핸들러 | FR-10, C-03, C-04 |
| Election voter 중복 방지 (`election_voters` unordered_set) | FR-10 |
| Election 성공(ACTIVE 전환) 시 peer 브로커에 `campus/alert/core_switch` 발행 → Edge 수신 가능 | FR-10, FR-14, A-03 |

#### Edge Broker (`broker/src/edge/main.cpp`)

| 구성요소 | 관련 FR/시나리오 |
| -------- | --------------- |
| UUID 자동 생성 (edge_id), outbound IP 자동 감지 (UDP SOCK_DGRAM) | — |
| mosq_core — Active Core 연결, LWT(W-02) 설정, 등록(M-03) publish | W-02, 5.3 |
| CT(M-04) 수신 → version 비교 후 구버전 무시, 최신 CT를 `ct_manager`에 반영 | M-04, FR-01, FR-09 |
| Ping/Pong 응답 (M-01/M-02) — 외부 Ping 수신 시 Pong 발행 | M-01, M-02 |
| mosq_local — 로컬 CCTV `campus/data/#` 구독, build_event_message() (타입·우선순위 자동 추론) | FR-01 |
| mosq_backup — Backup Core 선택적 연결 (`[backup_core_ip] [backup_core_port]`) | FR-05 |
| Core LWT(W-01) 수신 → prefer_backup 모드 전환, 저장 큐 즉시 flush 시도 | FR-05 |
| Store-and-Forward FIFO 큐 (std::deque, mutex 보호) | FR-07 |
| flush_store_queue() — Core 재연결 / Backup 연결 시 큐 재전송 | FR-07 |
| forward_message_upstream() — core_connected / backup_connected / prefer_backup 기반 failover 라우팅 | FR-05, FR-07 |
| on_connect_backup — Backup Core 등록 + 큐 flush | FR-05 |
| `campus/alert/core_switch` 수신 → `parse_ip_port` → `mosq_core` 재연결 (`active_core_ip/port` 갱신) | FR-05, A-03 |
| CT 수신 시 `active_core_id` 변경 감지 → `findNode` → 새 IP:Port로 `mosq_core` 재연결 | FR-05, FR-10 |
| CT 수신 후 ONLINE NODE에 Ping 발송 + 발송 시각 기록 (`ping_send_times`, `ping_mutex`) | FR-08, M-01 |
| Pong 수신 → RTT 계산 → `ct_manager.addLink()` RTT 갱신 | FR-08, M-02 |
| `select_relay_node()` — RTT 최소 + `hop_to_core` 최소 기준 `relay_node_id` 갱신 | FR-08, 5.6 |
| `campus/monitor/pong/<edge_id>` 구독 추가 (RTT 측정 수신용) | FR-08 |
| `campus/relay/<edge_id>` 구독 추가 → 수신 시 `campus/data/<source_id>`로 재발행 (인접 Edge 중계 요청 처리) | FR-08, R-01 |
| `forward_message_upstream()` relay fallback — 직접 경로 실패 시 `campus/relay/<relay_node_id>` 발행 | FR-08, R-01 |
| `on_connect_core()` 재연결 시 `last_ct_version = 0` 리셋 → 새 Core의 v1부터 수신 가능 | FR-01, FR-09 |

#### Web Client

| 구성요소 | 관련 FR |
| -------- | ------- |
| MQTT.js WebSocket 연결 + 7개 토픽 구독 | FR-11 |
| CT(topology) 수신 + Cytoscape 실시간 토폴로지 그래프 | FR-12 |
| `campus/data/#` 이벤트 수신 + 로그 표시, msg_id 클라이언트 중복 필터 | FR-11 |
| node_down / node_up 알림 처리 (A-01, A-02) | FR-13 |
| core_switch / LWT_CORE 수신 → Backup Core 재연결 배너 | FR-14 |

---

### 14.2 미완료 항목

#### 기타

| 구성요소 | 관련 FR | 비고 |
| -------- | ------- | ---- |
| CT NodeEntry에 WebSocket 포트 필드 추가 | FR-11 | failover 재연결 시 ws:// 포트 불일치 (현재 MQTT 포트 1883 저장, WS 포트 9001 필요) |
| Core 간 부하 정보 공유 (`_core/sync/load_info`) | FR-10 | Phase 3 이후 |

---

### 14.2.1 설계 제약 — 해소된 제약 현황

Phase 5+/6 구현으로 이전 설계 제약이 모두 해소되었다.

| 조건 | Edge가 새 Active Core로 재연결하는가? |
|---|---|
| Active SIGKILL → LWT → `core_switch` | ✅ `core_switch` 수신 → `parse_ip_port` → `mosq_core` 재연결 |
| Election → `core_switch` (peer 브로커 발행) | ✅ 동일 — peer 브로커에 발행된 `core_switch` 수신 → 재연결 |
| CT 수신 → `active_core_id` 변경 | ✅ `findNode` → 새 IP:Port로 `mosq_core` 재연결 |

---

### 14.3 개발 단계별 진행 현황

| Phase | 목표 | 관련 FR | 상태 |
| ----- | ---- | ------- | ---- |
| **1** | End-to-End 정상 동작 (CCTV → Edge → Core → Client) | FR-01, FR-02, FR-03, FR-06 | ✅ 완료 |
| **2** | Core 장애 대응 + Store-and-Forward | FR-04, FR-05, FR-07, FR-14 | ✅ 완료 |
| **3** | RTT 측정 + Relay 경로 선택 | FR-08 | ✅ 완료 |
| **4** | Web Client 이벤트 로그 + 토폴로지 그래프 | FR-11, FR-12 | ✅ 완료 |
| **5**  | Core 미완료 항목 (CT version 비교, Node 복구, Election) | FR-01, FR-10, FR-13 | ✅ 완료 |
| **5+** | Core Election 분산 환경 지원 (peer 채널 핸들러, voter 중복 방지, peer core_switch) | FR-10, FR-14 | ✅ 완료 |
| **6**  | Edge 재연결 (`core_switch` 수신 → 새 Active Core 재연결, CT `active_core_id` 변경 감지) | FR-05, FR-10 | ✅ 완료 |

### 14.4 Phase 5 / 5+ / 6 구현 완료 항목 (2026-04-17)

#### Core Broker

| 구성요소 | 관련 FR | 구현 위치 |
| -------- | ------- | --------- |
| CT 수신 시 version 비교 → 구버전 무시 (`on_message_peer`, `on_message`) | FR-01 | `broker/src/core/main.cpp` |
| Node 복구(OFFLINE→ONLINE) 감지 → `campus/alert/node_up/<id>` 발행 | FR-13, A-02 | `broker/src/core/main.cpp` |
| Core Election 메커니즘 — `_core/election/request` 수신 시 ID 비교 투표, `_core/election/result` 집계 후 ACTIVE 전환 | FR-10, C-03, C-04 | `broker/src/core/main.cpp` |
| `MSG_TYPE_ELECTION_REQUEST` / `ELECTION_RESULT` 타입 및 토픽 상수 추가 | FR-10 | `broker/include/message.h`, `broker/src/common/mqtt_json.cpp` |

#### Edge Broker

| 구성요소 | 관련 FR | 구현 위치 |
| -------- | ------- | --------- |
| CT 수신 후 version 비교 → 구버전 무시, 최신 CT를 `ct_manager`에 반영 | FR-01, FR-09 | `broker/src/edge/main.cpp` |

#### 시나리오 테스트 — Phase 5

| 스크립트 | 검증 내용 |
| -------- | --------- |
| `test/core/07_node_recovery.sh` | Node LWT → OFFLINE → M-03 재발행 → `campus/alert/node_up` 수신 확인 |
| `test/core/08_election.sh` | `_core/election/request` 발행 → `_core/election/result` 수신 → topology 갱신 확인 |
| `test/core/09_election_distributed.sh` | Active+Backup 2-core — Backup이 election 후 ACTIVE 전환, peer 브로커에 `campus/alert/core_switch` 발행 확인 |

#### Core Broker — Phase 5+ (분산 Election)

| 구성요소 | 관련 FR | 구현 위치 |
| -------- | ------- | --------- |
| `on_connect_peer()`에서 `TOPIC_ELECTION_ALL` 구독 추가 | FR-10 | `broker/src/core/main.cpp` |
| `on_message_peer()` election_request 핸들러 — peer 브로커에서 요청 수신 후 투표 발행 | FR-10, C-03 | `broker/src/core/main.cpp` |
| `on_message_peer()` election_result 핸들러 — voter 중복 방지 + 과반 시 ACTIVE 전환 + peer 브로커에 `core_switch` 발행 | FR-10, C-04, FR-14 | `broker/src/core/main.cpp` |
| `election_voters` unordered_set — own/peer 채널 중복 집계 방지 | FR-10 | `broker/src/core/main.cpp` |

#### Edge Broker — Phase 6 (재연결)

| 구성요소 | 관련 FR | 구현 위치 |
| -------- | ------- | --------- |
| `EdgeContext`에 `active_core_ip/port` 필드 추가 (현재 연결 Core 주소 추적) | FR-05 | `broker/src/edge/main.cpp` |
| `on_connect_core()`: `campus/alert/core_switch` 구독 추가 | FR-05 | `broker/src/edge/main.cpp` |
| `on_message_core()`: `core_switch` 수신 → `parse_ip_port` → `mosq_core` 재연결 | FR-05, A-03 | `broker/src/edge/main.cpp` |
| `on_message_core()`: CT 수신 시 `active_core_id` 변경 감지 → `findNode` → 새 IP:Port로 재연결 | FR-05, FR-10 | `broker/src/edge/main.cpp` |

#### 시나리오 테스트 — Phase 6

| 스크립트 | 검증 내용 |
| -------- | --------- |
| `test/edge/03_core_switch.sh` | `campus/alert/core_switch` 수신 → Edge 재연결 시도 로그 확인 |
| `test/edge/04_ct_active_core_change.sh` | CT v1→v2 `active_core_id` 변경 감지 후 재연결 시도 확인 |

#### 통합 테스트 (Python/pytest) — `test/integration/`

paho-mqtt 2.x + pytest 기반 통합 테스트. 실제 바이너리(`core_broker`, `edge_broker`)를 subprocess로 기동해 Client-Core-Edge 간 상호작용을 end-to-end 검증한다.

| 파일 | 시나리오 | 검증 항목 | 관련 FR |
| ---- | -------- | --------- | ------- |
| `test_01_e2e.py` | 이벤트 종단간 전달 | Publisher → `campus/data/INTRUSION` → Core 재발행 → Spy 수신, 필드(type·priority·payload) 보존 | FR-01, FR-03 |
| `test_02_failover.py` | Core 페일오버 | Active SIGKILL → LWT → Backup이 `campus/alert/core_switch` 발행, Edge 로그에 재연결 시도 확인 | FR-04, FR-05, FR-14 |
| `test_03_node_lifecycle.py` | Node 장애·복구 | LWT 시뮬 → `campus/alert/node_down` OFFLINE 수신, 재등록 → `campus/alert/node_up` ONLINE 수신 | FR-06, FR-13 |
| `test_04_dedup.py` | 이벤트 중복 방지 | 동일 `msg_id` 2회 발행 → Core 로그 `event forwarded` 1회, 다른 `msg_id` → 각각 재발행 | FR-02 |
| `test_05_ct_sync.py` | CT 동기화 | 노드 등록 후 `_core/sync/connection_table` 갱신·발행, CT에 node_id 포함, version 증가, Backup 수신 확인 | FR-01, C-01 |
| `test_06_rtt_relay.py` | RTT 측정·Relay 선택 | Core+Edge×2 기동 → CT 수신 후 Ping 발송, Pong 수신 RTT 계산, relay node 선택, OFFLINE node 후보 제외 확인 | FR-08 |
| `test_09_relay_ack.py` | Relay ACK 검증 (H-3, R-02) | Core가 이벤트 처리 후 `campus/relay/ack/<msg_id>` 발행 확인, 중복 msg_id dedup, Backup Core relay/ack 포함 4개 케이스 | FR-02, R-02 |
| `test_10_core_rejoin.py` | Core ID 영속화 검증 | 최초 실행 시 `/tmp/core_id_<port>.txt` 생성, 재시작 후 동일 UUID 유지, Backup 모드 재진입 시 UUID 연속성 보장 | 15.1.1 |

공통 인프라 (`test/integration/conftest.py`):

| 구성요소 | 역할 |
| -------- | ---- |
| `MqttSpy` | 지정 토픽 구독·누적, `wait_for` / `wait_payload` / `count` 제공 |
| `run_proc()` | 바이너리 subprocess 기동, startup log 패턴 대기, 컨텍스트 종료 시 SIGTERM |
| `active_core` / `backup_core` / `edge` fixtures | 각 바이너리 기동·정리 자동화 |
| `make_event()` / `make_status_msg()` | 이벤트·STATUS 메시지 dict 생성 헬퍼 |
| `wait_log()` | 로그 파일에서 regex 패턴 대기 |
| `_clear_retained()` / `clear_retained_before_session` | 테스트 세션 전후 retained 메시지(`campus/monitor/topology`, `_core/sync/connection_table`) 초기화 — 잔류 CT로 인한 version 충돌 방지 |
| `active_core` fixture retained 클리어 | `active_core` 픽스처 setup/teardown 시 retained 클리어 → stale CT 버전 오염 방지 |
| `_clear_core_id_file()` / `active_core`·`backup_core` 픽스처 통합 | 테스트 전 `/tmp/core_id_<port>.txt` 삭제 → Active·Backup이 동일 포트 공유 시 UUID 충돌 방지 |

공통 인프라 (`test/lib/common.sh`):

| 구성요소 | 역할 |
| -------- | ---- |
| `cleanup()` — `pkill -x core_broker / edge_broker` | 등록되지 않은 잔류 broker 프로세스도 강제 종료 — 이전 테스트의 ghost 프로세스가 stale CT를 재발행하는 문제 방지 |
| `cleanup()` — retained 메시지 초기화 | 종료 시 `campus/monitor/topology`, `_core/sync/connection_table` retained 클리어 |

### 14.5 Phase 3 구현 완료 (2026-04-17)

| 구성요소 | 위치 | 내용 |
| -------- | ---- | ---- |
| `edge_helpers.h` | `broker/include/` | `infer_msg_type`, `infer_priority`, `parse_building_camera`, `select_relay_node` 순수 로직 헬퍼 (mosquitto 의존 없음) |
| Ping 발송 트리거 | `edge/main.cpp` on_message_core (CT 수신 후) | CT의 ONLINE NODE 순회 → Ping 발송 + 발송 시각 기록 (`ping_send_times`) |
| RTT 측정 | `edge/main.cpp` on_message_core (Pong 수신 시) | 발송 시각 차이 계산 → `ct_manager.addLink()` RTT 갱신 |
| Relay 경로 선택 | `select_relay_node()` (`edge_helpers.h`) | RTT 최소 선택, RTT 동점 시 `hop_to_core` 최소 선택, `relay_node_id` 갱신 |
| 단위 테스트 | `broker/test/test_edge_logic.cpp` | TC-01~TC-08 (32 assertions) — CMake `test_edge_logic` 타겟 등록 |
| 통합 테스트 | `test/integration/test_06_rtt_relay.py` | TC-01~TC-04 — Ping 발송·RTT 계산·Relay 선택·OFFLINE 제외 end-to-end 검증 |
| 쉘 테스트 | `test/edge/05_rtt_relay.sh` | 2-Edge 환경에서 RTT 측정·Relay 선택 로그 패턴 확인 |

---

## 15. 구현 검토 결과 및 미비 사항 (2026-04-19 기준)

> 코드 전체 정적 분석 + 통합 테스트 25개 통과 이후 수행한 심층 검토 결과.
> **현재 시스템은 2-core + localhost 환경의 통제된 테스트에서 정상 동작하나, 실제 배포 환경에서 발생할 수 있는 미비 사항을 우선도 순으로 정리한다.**

---

### 15.1 우선도 HIGH — 이벤트 유실 가능성

#### H-1: `connected` 플래그와 실제 소켓 상태 불일치 (Race Condition) ✅ 완료

| 항목 | 내용 |
|------|------|
| **위치** | `broker/src/edge/main.cpp` — `on_message_upstream()` 내 core_switch 처리부 및 CT active_core_id 변경 감지부 |
| **현상** | `mosquitto_disconnect()` 직후 `up.connected` 플래그를 `false`로 세팅하지 않음 |
| **실패 시나리오** | Core IP 변경 감지 → `disconnect()` 호출 후 `connect_async()` 완료 전 인입 이벤트가 `connected=true` 상태의 죽은 소켓으로 발행됨 → MQTT 레이어에서 조용히 유실 |
| **재현 조건** | Core failover 중 고빈도 이벤트 발행 시 |
| **조치** | `mosquitto_disconnect()` 호출 직전 `core_slot->connected = false` 세팅 — core_switch 수신부(L743), CT active_core_id 변경부(L660) 두 곳 모두 적용 |

#### H-2: NIC 장애 시 Zombie TCP 연결 미감지 ✅ 완료

| 항목 | 내용 |
|------|------|
| **위치** | Edge ↔ Core upstream 연결 전반 |
| **현상** | Core 프로세스는 살아있으나 NIC가 다운되면 TCP 소켓은 정상처럼 보임. MQTT CONNACK 성공 이후 데이터만 흐르지 않는 상태 |
| **실패 시나리오** | Edge가 Core를 ONLINE으로 판단해 계속 publish → 메시지는 커널 send buffer에 적체 → store-and-forward 큐는 비어있음 → 네트워크 복구 후 오래된 메시지가 뒤늦게 전달되거나 retransmit 타임아웃 후 유실 |
| **재현 조건** | NIC 레벨 장애 (라우터 다운, 케이블 단절 등) |
| **조치** | mosquitto keepalive 60s → 10s로 단축 적용 — edge/main.cpp 5곳(Core·Peer·Backup upstream), core/main.cpp 2곳(mosq, mosq_peer). mosq_local(localhost)은 제외 |

#### H-3: PUBACK 수신 전 Core 크래시 시 이벤트 유실 ✅ 완료

| 항목 | 내용 |
|------|------|
| **위치** | Edge → Core QoS 1 publish 경로 전반 |
| **현상** | QoS 1은 PUBACK 수신 전까지 재전송을 보장하지만, Core가 PUBACK 전에 크래시하면 Edge는 재연결 후 해당 메시지를 재전송하지 않음 (mosquitto 세션 클린 플래그 = true로 추정) |
| **실패 시나리오** | Core 처리 직전 크래시 → Edge 재연결 후 새 세션 시작 → 이전 미확인 메시지 소실 |
| **조치** | R-02 (`campus/relay/ack/<msg_id>`) application-level ACK 활용 — Core가 이벤트 처리 후 ack 발행, Edge는 ack 수신 전까지 pending_msgs에 보관, CORE/BACKUP disconnect 시 pending → store_queue 앞에 재삽입 → flush 시 재전송. Core의 seen_msg_ids dedup으로 중복 전달 안전 처리. |

---

### 15.1.1 Active Core Backup 재진입 문제 ✅ 완료

| 항목 | 내용 |
|------|------|
| **위치** | `broker/src/core/main.cpp` — main() 초기화부 |
| **현상** | Active Core SIGKILL 후 재시작 시 `uuid_generate()`로 새 UUID 생성 → CT 내 이전 core_id와 불일치, Edge/Backup이 "다른 노드"로 인식 |
| **실패 시나리오** | A(Active) SIGKILL → B(Backup→Active 승격) → A 재시작 시 새 UUID → CT에서 A가 전혀 다른 노드로 등록, backup_core_id 불일치 |
| **조치** | core_id 파일 영속화 (`/tmp/core_id_<port>.txt`) — 최초 실행 시 UUID 생성 후 저장, 재시작 시 파일에서 읽어 동일 UUID 재사용. 기존 `merge_backup_registration`이 동일 UUID 기준으로 backup_core_id 갱신하므로 재진입 시 CT continuity 자동 보장. |

---

### 15.2 우선도 MEDIUM — 연결 안정성

#### M-1: flush_store_queue 첫 항목 실패 시 나머지 큐 중단

| 항목 | 내용 |
|------|------|
| **위치** | `broker/src/edge/main.cpp` — `flush_store_queue()` |
| **현상** | 큐 첫 항목 전송 실패 시 `return`으로 즉시 종료. 이후 항목들은 다음 `flush` 호출까지 대기 |
| **실패 시나리오** | 큐에 100개 이벤트 적체 → 첫 항목 전송 중 네트워크 순간 끊김 → 전체 flush 중단 → 재연결 후 flush 재시작 필요 → 복구 지연 |
| **조치** | 실패한 항목을 큐 맨 앞으로 반환하되, 나머지 항목은 계속 flush 시도. 또는 지수 백오프 후 전체 재시도 |

#### M-2: relay_node_id 비보호 concurrent access

| 항목 | 내용 |
|------|------|
| **위치** | `broker/src/edge/main.cpp` — `process_pong()` 내 `strncpy(ctx->relay_node_id, ...)` |
| **현상** | `ping_mutex`를 해제한 이후 `relay_node_id`를 갱신. `forward_message_upstream()`에서 락 없이 `relay_node_id` 읽음 |
| **실패 시나리오** | 메시지 핸들러와 pong 핸들러가 동시에 동작할 때 반쪽만 쓰인 UUID가 사용될 수 있음 |
| **조치** | `relay_node_id` 갱신 및 읽기 모두 `ping_mutex` 또는 별도 mutex로 보호 |

#### M-3: 로컬 브로커 재시작 시 Edge 바이너리 재진입 없이 종료

| 항목 | 내용 |
|------|------|
| **위치** | `broker/src/edge/main.cpp` — `main()` 로컬 브로커 초기 연결부 |
| **현상** | `mosquitto_connect(mosq_local, ...)` 실패 시 `return 1`로 바이너리 종료. 재시도 로직 없음 |
| **실패 시나리오** | 로컬 mosquitto 브로커가 잠시 재시작되는 동안 Edge 바이너리가 시작되면 즉시 종료됨 |
| **조치** | `mosquitto_reconnect_delay_set()` 으로 재연결 루프 구성, 또는 초기 연결에도 재시도 루프 적용 |

#### M-4: 포트 TIME_WAIT로 인한 빠른 재시작 실패

| 항목 | 내용 |
|------|------|
| **위치** | OS 레벨 — Core/Edge 바이너리 재시작 시 |
| **현상** | 프로세스 종료 후 OS가 해당 포트를 TIME_WAIT(30~120s) 상태로 유지. 동일 포트 재바인딩 시 EADDRINUSE |
| **실패 시나리오** | Core SIGKILL → 즉시 재시작 시도 → "Address already in use" 오류로 재시작 실패 |
| **재현 조건** | 테스트에서는 pytest fixture teardown에 `sleep(0.5)` 회피 처리가 되어있으나, 실제 배포에선 서비스 재시작 스크립트에서 동일 문제 발생 |
| **조치** | mosquitto 또는 서비스 매니저 레벨에서 `SO_REUSEADDR` 옵션 적용 확인, 또는 재시작 스크립트에 충분한 대기 추가 |

#### M-5: Backup Core가 peer 연결 대상을 정적으로만 참조

| 항목 | 내용 |
|------|------|
| **위치** | `broker/src/core/main.cpp` — `main()` Backup 초기화부 |
| **현상** | Backup Core는 최초 설정된 `active_core_ip:port`로만 peer 재연결 시도. Active Core IP가 failover 중 변경되어도 Backup은 갱신된 주소를 알 수 없음 |
| **실패 시나리오** | 3-core 구성에서 Core A(Active) → Core C(새 Active)로 전환 후 Core B(Backup)이 Core A에게 계속 peer 연결 시도 |
| **조치** | Election 결과로 발행되는 `core_switch` 토픽을 Backup도 수신하여 peer 연결 대상 갱신 |

---

### 15.3 우선도 MEDIUM — FR 미완성 사항

#### F-1: Election이 2-core 환경에서만 안전 (FR-10 부분 구현)

| 항목 | 내용 |
|------|------|
| **위치** | `broker/src/core/main.cpp` — election_result 핸들러 |
| **현상** | `election_votes >= 1`이면 즉시 ACTIVE 전환. 2-core에서는 유효하나 3+ core에서는 과반수 미달 상태에서 다수 Core가 동시에 ACTIVE 선언 가능 |
| **추가 이슈** | `election_winner` 필드는 코드에서 정의되어 있으나 실제 사용되지 않음 (다중 후보 투표 로직 미완성) |
| **조치** | 전체 Core 수를 CT에서 카운팅하여 과반수(n/2 + 1) 임계값으로 전환, election timeout 추가 |

#### F-2: QoS 1 end-to-end 보장 불완전 (FR-03 부분 구현)

| 항목 | 내용 |
|------|------|
| **현상** | Edge → Core 구간은 MQTT QoS 1이지만, Core가 수신 후 Client로 republish하는 구간의 ACK 추적은 없음. Core 크래시 시 "수신했지만 republish 못한" 이벤트 유실 가능 |
| **조치** | 프로젝트 제약(시험 범위) 내에서는 허용 가능. 단, PRD에 명시적 제약으로 문서화 |

#### F-3: 이벤트 큐가 메모리 전용 — Edge 재시작 시 유실 (FR-07 부분 구현)

| 항목 | 내용 |
|------|------|
| **위치** | `broker/src/edge/main.cpp` — `EdgeContext::store_queue` (std::deque) |
| **현상** | Store-and-Forward 큐가 프로세스 메모리에만 존재. Edge 바이너리 크래시 또는 재시작 시 적체된 이벤트 전량 유실 |
| **조치** | 파일 기반 영속 큐(SQLite, LMDB, 또는 단순 append-only 파일)로 교체. 단, 구현 복잡도 증가 고려 필요 |

---

### 15.4 우선도 LOW — 장기 안정성

#### L-1: Edge ID가 IP 기반 결정론적 생성 — DHCP 환경에서 UUID 변경

| 항목 | 내용 |
|------|------|
| **위치** | `broker/src/edge/main.cpp` — `main()` edge_id 생성부 |
| **현상** | edge_id = `UUID(hash("edge:<ip>:<port>"))`. IP가 바뀌면 edge_id가 바뀌고 Core CT에 유령 노드 누적 |
| **조치** | 최초 실행 시 UUID를 생성해 파일에 저장(`~/.edge_id` 등), 이후 재사용 |

#### L-2: Pong 응답 토픽이 relay 경유 시 불일치 가능성

| 항목 | 내용 |
|------|------|
| **위치** | `broker/src/edge/main.cpp` — `process_ping()` |
| **현상** | Pong은 `campus/monitor/pong/<ping.source.id>`로 발행됨. Ping이 peer relay를 경유하면, Pong이 발행되는 브로커가 Pong 구독자의 브로커와 다를 수 있음 |
| **재현 조건** | EdgeC(peer-only) → EdgeA(relay) → Core 구성에서 EdgeA가 EdgeC에게 Ping 전송 시 |
| **조치** | Pong 토픽에 발행 브로커 경로 정보를 포함하거나, Pong을 Core 경유로 발행하도록 변경 |

#### L-3: 이벤트 타임스탬프 클록 신뢰성 없음

| 항목 | 내용 |
|------|------|
| **현상** | 모든 이벤트 타임스탬프는 Edge 시스템 클록 기준 UTC. NTP 동기화 미확인 시 수 시간 오차 발생 가능 |
| **조치** | 배포 시 NTP 동기화 전제를 시스템 요구사항에 명시, 또는 Core 수신 시각을 추가 필드로 기록 |

---

### 15.5 현재 테스트 환경과 실제 배포 환경 간 전제 차이

아래 조건들은 통합 테스트에서는 충족되어 있으나 실제 배포 시 별도 확인이 필요하다.

| 전제 조건 | 테스트 환경 | 실제 배포 |
|-----------|-------------|-----------|
| Core 수 | 항상 2개 (Active + Backup) | 3+ 가능 시 election 로직 재검토 필요 |
| 네트워크 | localhost (지연 < 1ms) | WAN 환경에서 reconnect/timeout 파라미터 재조정 필요 |
| 브로커 재시작 타이밍 | fixture teardown에 sleep(0.5) 회피 처리 | 실제 서비스 재시작 스크립트에 TIME_WAIT 대기 추가 필요 |
| 이벤트 큐 | 메모리 전용 (재시작 시 유실) | 영속 큐 또는 재시작 정책 수립 필요 |
| 클록 동기화 | 단일 호스트이므로 무관 | NTP 동기화 필수 |
| local broker 가용성 | 테스트 시작 전 mosquitto 기동 보장 | 오케스트레이션 환경에서 startup 순서 보장 필요 |

---

### 15.6 Phase 7 — 미비 사항 반영 후보 항목

아래는 우선도에 따라 이후 Phase에서 반영 가능한 항목이다.

| 항목 | 우선도 | 예상 작업량 | 관련 이슈 |
|------|--------|-------------|-----------|
| ~~disconnect 즉시 `connected=false` 세팅~~ | ~~HIGH~~ | ~~소 (1줄)~~ | H-1 ✅ |
| ~~mosquitto keepalive 10s로 단축~~ | ~~HIGH~~ | ~~소~~ | H-2 ✅ |
| ~~R-02 relay/ack application-level ACK (pending_msgs + requeue)~~ | ~~HIGH~~ | ~~중~~ | H-3 ✅ |
| ~~core_id 파일 영속화 (`/tmp/core_id_<port>.txt`)~~ | ~~HIGH~~ | ~~소~~ | 15.1.1 ✅ |
| flush_store_queue 첫 실패 시 나머지 계속 flush | MEDIUM | 소~중 | M-1 |
| relay_node_id mutex 보호 | MEDIUM | 소 | M-2 |
| 로컬 broker 초기 연결 재시도 루프 | MEDIUM | 중 | M-3 |
| Election quorum = n/2+1 동적 계산 | MEDIUM | 중~대 | F-1 |
| election_winner 필드 실제 다중 후보 투표에 사용 | MEDIUM | 대 | F-1 |
| Edge ID 파일 영속화 | LOW | 소~중 | L-1 |

---

## 13. 부록

### 13.1 팀 정보

| 역할 | 이름   | 학번     |
| ---- | ------ | -------- |
| 팀원 | 김민혁 | 21900103 |
| 팀원 | 추인규 | 22000771 |
| 팀원 | 권혁민 | 22100061 |

### 13.2 참고 문서

- MQTT v3.1.1 Specification (OASIS)
- IoT 중간 프로젝트 중간발표.pdf (2조, 2025)
- Mosquitto MQTT Broker Documentation
- Cytoscape.js Graph Visualization Library
