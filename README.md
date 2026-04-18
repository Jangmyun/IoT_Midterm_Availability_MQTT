# IoT_Midterm_Availability_MQTT

스마트 캠퍼스 환경에서 CCTV 이벤트를 고가용성으로 전달하는 분산 MQTT 브로커 시스템입니다.

## 1. 프로젝트 개요

단일 Core 브로커 장애, 네트워크 불안정, 이벤트 중복 등의 문제를 해결하기 위해 설계된 **고가용성 분산 MQTT 브로커** 시스템입니다.

**핵심 기능:**
- Active/Backup Core 이중화 및 자동 페일오버
- Edge 브로커의 Store-and-Forward 큐 (오프라인 내성)
- UUID 기반 이벤트 중복 제거
- RTT 측정 기반 최적 릴레이 경로 선택
- Connection Table 버전 동기화 (Active ↔ Backup)
- 분산 리더 선출 (Core 역할 결정)

**기술 스택:** C++ (브로커), React + MQTT.js (웹 클라이언트), Python pytest (통합 테스트), Bash (셸 테스트)

---

## 2. 시스템 아키텍처

```
┌─────────────────────────────────────────────────────────┐
│                    Web Clients                          │
│   [Monitoring Dashboard]        [Publisher Simulator]  │
│   React + Cytoscape + MQTT.js   React + MQTT.js        │
└──────────────────────┬──────────────────────────────────┘
                       │ WebSocket (:9001)
┌──────────────────────▼──────────────────────────────────┐
│              Mosquitto MQTT Broker (:1883)               │
└──────────────────────┬──────────────────────────────────┘
                       │
        ┌──────────────┴──────────────┐
        │        CORE LAYER           │
        │  Core A (Active)  ←sync→   │
        │  Core B (Backup)            │
        │  (LWT, CT sync, election)   │
        └──────────────┬──────────────┘
                       │
     ┌─────────────────┼─────────────────┐
     │                 │                 │
┌────▼────┐      ┌─────▼────┐      ┌────▼────┐
│ Edge A  │─RTT─▶│ Edge B   │─RTT─▶│ Edge C  │
│ (건물A) │      │ (건물B)   │      │ (건물C) │
└────┬────┘      └─────┬────┘      └────┬────┘
     │                 │                 │
  CCTV(Pub)         CCTV(Pub)        CCTV(Pub)
```

### 컴포넌트 역할

| 컴포넌트 | 언어 | 역할 |
|---------|------|------|
| **Core Broker** | C++ | Connection Table 관리, Active/Backup 이중화, 이벤트 집계 |
| **Edge Broker** | C++ | CCTV 이벤트 수집, Store-and-Forward 큐, RTT 기반 릴레이 선택 |
| **Monitoring Client** | React | 실시간 토폴로지 시각화, 이벤트 스트림, 페일오버 알림 |
| **Publisher Client** | React | CCTV 이벤트 시뮬레이션 (테스트/데모용) |

### 주요 MQTT 토픽

| 토픽 | 발행자 | 설명 |
|------|--------|------|
| `campus/monitor/topology` | Core | 네트워크 토폴로지 (retained) |
| `campus/data/#` | Edge/Pub | CCTV 이벤트 데이터 |
| `campus/relay/#` | Edge | 릴레이 경로 포워딩 |
| `campus/will/core/#` | Core | Core LWT |
| `campus/will/node/#` | Edge | Edge LWT |
| `_core/sync/connection_table` | Core(Active) | CT 동기화 (retained) |
| `_core/election/#` | Core | 리더 선출 투표 |

---

## 3. 작동 방식

### 사전 요구사항

- CMake ≥ 3.20, C++17 컴파일러
- Node.js & npm
- mosquitto, libmosquitto, libcjson, nlohmann/json
- Python 3 + pytest, paho-mqtt (통합 테스트용)

### 의존성 설치 및 빌드

```bash
# 의존성 설치 + 브로커 빌드 + npm install
bash install.sh --build

# 빌드만 다시 실행
npm run broker:build

# 브로커 빌드 (수동)
cd broker
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

### 실행 순서

**1. Mosquitto 기본 브로커 시작**

```bash
mosquitto -c mosquitto.conf
# MQTT: 1883, WebSocket: 9001
```

**2. Core Broker 실행**

```bash
# Active Core
./broker/build/core_broker \
  --core-id <uuid> \
  --core-ip 127.0.0.1 \
  --core-port 1883 \
  --broker-ip 127.0.0.1 \
  --broker-port 1883

# Backup Core
./broker/build/core_broker \
  --core-id <uuid> \
  --is-backup \
  --active-ip 127.0.0.1 \
  --active-port 1883 \
  --broker-ip 127.0.0.1 \
  --broker-port 1884
```

**3. Edge Broker 실행**

```bash
./broker/build/edge_broker \
  --edge-id <uuid> \
  --node-ip 127.0.0.1 \
  --node-port 2883 \
  --core-ip 127.0.0.1 \
  --core-port 1883
```

**4. 웹 클라이언트 실행**

```bash
npm run client:dev      # 모니터링 대시보드 (기본 :5173)
npm run publisher:dev   # 퍼블리셔 시뮬레이터 (기본 :5174)
```

### 테스트

```bash
# C++ 유닛 테스트
npm run broker:build   # 빌드 + ctest 포함

# 셸 스크립트 테스트 (PUB-01~08, EC-01~04, CORE-01~02, EDGE-01~02)
cd test && bash test_pub.sh <TEST_ID>

# Python 통합 테스트
pytest test/integration/
```

### npm 스크립트 요약

| 명령어 | 설명 |
|--------|------|
| `npm run broker:configure` | CMake 구성 |
| `npm run broker:build` | 브로커 빌드 + 유닛 테스트 |
| `npm run broker:clean` | 빌드 디렉토리 삭제 |
| `npm run client:dev` | 모니터링 클라이언트 개발 서버 |
| `npm run client:build` | 모니터링 클라이언트 프로덕션 빌드 |
| `npm run publisher:dev` | 퍼블리셔 클라이언트 개발 서버 |
| `npm run publisher:build` | 퍼블리셔 클라이언트 프로덕션 빌드 |
