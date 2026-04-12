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
4. Publisher: Connection Table 참조 → 인접 정상 Node로 이벤트 릴레이

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
| **FR-07** | 전송 실패 이벤트를 로컬 FIFO 큐에 저장 후 재전송 (Store-and-Forward)              | P0       | Node 중단           |
| **FR-08** | RTT 및 `hop_to_core` 기반 최적 Relay Node 선택                                    | P1       | Relay 선택          |
| **FR-09** | 갱신된 Connection Table을 Retained Message로 신규 Broker에 전달                   | P1       | 신규 Broker 연결    |
| **FR-10** | Core 간 부하 정보 주기적 공유 및 RTT 기반 Active Core 선출                        | P1       | 트래픽 분산         |
| **FR-11** | MQTT.js 기반 웹 클라이언트에서 Core Broker WebSocket 연결 및 이벤트 구독          | P1       | 모니터링 클라이언트 |
| **FR-12** | Graph Visualization 라이브러리로 Connection Table 변경 상태 실시간 표시           | P1       | 모니터링 클라이언트 |
| **FR-13** | Node 복구 완료 시 `A-02` 토픽으로 Client에 복구 알림 전송                         | P2       | Node 중단           |
| **FR-14** | Active Core 변경 시 `A-03` 토픽으로 Client 재연결 유도                            | P2       | Core 중단           |

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
