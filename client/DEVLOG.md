# Client 개발 로그

> client 측 구현 변경 사항만 기록. 브로커(C++) 변경은 제외.

---

## [2026-04-13] 초기 구현 — ConnectionTable JSON 파싱

### 신규 파일

#### `src/mqtt/parsers.js`
- `parseConnectionTable(raw)` 구현
- `campus/monitor/topology` (M-04) 토픽에서 수신한 ConnectionTable JSON 파싱
- C++ `connection_table_from_json()`과 필드 이름 1:1 대응
- 필수 필드 검증: `version`(number), `nodes`(array), `links`(array)
- 파싱 실패 시 `null` 반환

#### `src/hooks/useMqtt.js` (v1)
- mqtt.js v5 WebSocket 연결 (`ws://localhost:9001`, 환경변수 `VITE_MQTT_URL`로 오버라이드 가능)
- `campus/monitor/topology` 단일 토픽 구독 (QoS 1)
- 반환값: `{ status, topology }`
- React StrictMode 대응: `useRef`로 클라이언트 보관 + cleanup `client.end(true)`

### 수정 파일

#### `src/App.jsx` (boilerplate → 모니터링 UI)
- 헤더: 연결 상태 배지 + Active/Backup Core ID + CT 버전 + 마지막 업데이트 시각
- Nodes 테이블: id, role, ip:port, status, hop_to_core
- Links 테이블: from_id, to_id, rtt_ms

#### `src/App.css`
- 다크 테마 모니터링 스타일
- `.status--connected/connecting/disconnected/error` 배지 색상
- `.row--offline` (OFFLINE 노드 투명도 45%)

---

## [2026-04-13] 전체 로직 구현 — 이벤트 파싱 / Cytoscape / Core 재연결

### 신규 파일

#### `src/components/TopologyGraph.jsx`
- Cytoscape.js 토폴로지 그래프 컴포넌트
- `topology` prop 변경 시 그래프 요소만 교체 (인스턴스 재생성 없음)
- 노드 스타일:
  - `CORE` → 보라색 다이아몬드 (`#6366f1`, shape: diamond, 44px)
  - `NODE ONLINE` → 초록 원 (`#4caf50`)
  - `NODE OFFLINE` → 빨강 반투명 (`#f44336`, opacity 0.55)
- 엣지: RTT(ms) 라벨 표시
- 레이아웃: `cose` (물리 시뮬레이션 기반 자동 배치)

### 수정 파일

#### `src/mqtt/parsers.js` — `parseMqttMessage()` 추가
- 적용 토픽: `campus/data/#`, `campus/alert/#`, `campus/will/core/#`
- 모두 MqttMessage JSON 형식 (브로커 팀 확정)
- C++ `mqtt_message_from_json()`과 필드 이름 1:1 대응
- `priority` 필드는 optional (`?? null` 처리)
- 파싱 실패 시 `null` 반환

#### `src/hooks/useMqtt.js` (v2) — 전면 확장
구독 토픽 추가:

| 토픽 | 용도 |
|------|------|
| `campus/data/+` | D-01 CCTV 이벤트 |
| `campus/data/+/+` | D-02 건물별 이벤트 |
| `campus/alert/node_down/+` | A-01 Node OFFLINE 알림 |
| `campus/alert/node_up/+` | A-02 Node 복구 알림 |
| `campus/alert/core_switch` | A-03 Active Core 교체 → 재연결 트리거 |
| `campus/will/core/+` | W-01 Core LWT (비정상 종료) → 재연결 트리거 |

주요 로직:
- **CT version guard**: 수신된 CT version이 현재 이하면 무시 (오래된 retained message 방지)
- **msg_id 중복 제거**: `useRef(new Set())`으로 동일 msg_id 재수신 시 무시
- **events**: 최신 50개 유지 (prepend + slice)
- **alerts**: 최신 20개 유지, 수신 5초 후 자동 제거 (`setTimeout`)
- **Core 재연결**: W-01/A-03 수신 시 `topology.backup_core_id`로 backup 노드 조회 → `ws://ip:port`로 `brokerUrl` state 변경 → useEffect 재실행으로 자동 재연결
- `topologyRef` 추가: message 핸들러 클로저 내에서 최신 topology 참조용

반환값 확장: `{ status, topology, events, alerts }`

#### `src/App.jsx` (v2) — 전체 UI 재구성

레이아웃:
```
헤더 (연결 상태 + Core 정보)
Alert 배너 (node_down/up/core_switch, 5초 자동 소멸)
Stats row (Online | Offline | Events | Critical)
Cytoscape 토폴로지 그래프
하단 2열: Broker 카드 목록 | 이벤트 로그
```

Broker 카드:
- CORE 우선 → ONLINE → OFFLINE 순 정렬
- 카드: role 배지 + status 배지 + UUID 앞 8자 + IP:Port + hop_to_core

이벤트 로그:
- 최신순 스크롤 리스트
- 항목: 시간(HH:MM:SS) + type 배지 + priority 배지(HIGH만) + building_id + description
- type 배지 색: `INTRUSION`=빨강, `DOOR_FORCED`=주황, `MOTION`=노랑, `LWT_*`=보라, 나머지=회색

#### `src/App.css` (v2)
- `.stats-row` / `.stat-card` — 통계 카드 4개
- `.topology-graph` — Cytoscape 컨테이너 (360px 고정 높이)
- `.bottom-row` — 하단 2열 flex (800px 이하에서 1열로 전환)
- `.broker-card` / `.broker-card--offline` — 브로커 상태 카드
- `.event-list` / `.event-item` — 이벤트 로그
- `.alert-banner` / `.alert-item--down/up/core` — alert 배너
- `.badge--green/red/orange/yellow/purple/gray` — 범용 배지

---

## [2026-04-13] UI 개선 3종 — Core 재연결 피드백 / 이벤트 필터링 / 노드 클릭 팝업

### 수정 파일

#### `src/hooks/useMqtt.js` (v3)
- `reconnectInfo` state 추가 (`{ url, reason } | null`)
- `connect` 이벤트 시 `setReconnectInfo(null)` — 연결 완료 시 자동 클리어
- `reconnectToBackup(reason)` — `'W-01'` / `'A-03'` reason 파라미터 추가
- 반환값 확장: `{ status, topology, events, alerts, reconnectInfo }`

#### `src/components/TopologyGraph.jsx` (v2)
- `onNodeClick` prop 추가, `onNodeClickRef`로 보관 (클로저 stale 방지)
- `cy.on('tap', 'node', ...)` — 노드 클릭 시 `onNodeClick(id)` 호출
- `cy.on('tap', evt)` — 배경 클릭 시 `onNodeClick(null)` 호출
- `node:selected` 스타일 추가 (흰 테두리 3px)

#### `src/App.jsx` (v3)
- `reconnectInfo` 수신 → Core failover 배너 (스피너 + 연결 대상 URL 표시)
- `filterType` / `filterBuilding` / `filterHighOnly` 3개 필터 state
  - type 목록 / building 목록은 수신된 events에서 동적 추출
  - 필터 조건 하나라도 활성 시 reset 버튼 등장
  - 헤더: `Event Log (필터 결과수/전체수)` 표시
- `selectedNodeId` state → topology.nodes 조회 → 그래프 하단 상세 패널
  - 전체 UUID, role/status 배지, IP:Port, hop_to_core, ✕ 닫기 버튼

#### `src/App.css` (v3)
- `.reconnect-banner`, `.reconnect-spinner` (스피너 애니메이션)
- `.event-filters`, `.filter-select`, `.filter-btn`, `.filter-btn--active`, `.filter-btn--reset`
- `.node-detail`, `.node-detail-header`, `.node-detail-title`, `.node-detail-close`
- `.node-detail-body`, `.node-detail-kv`

---

## 현재 파일 구조

```
client/src/
├── mqtt/
│   └── parsers.js           parseConnectionTable(), parseMqttMessage()
├── hooks/
│   └── useMqtt.js           MQTT 연결 + 전체 토픽 + Core 재연결 + reconnectInfo
├── components/
│   └── TopologyGraph.jsx    Cytoscape 토폴로지 그래프 + 노드 클릭 콜백
├── App.jsx                  메인 UI (필터링 + 재연결 배너 + 노드 상세 패널)
├── App.css                  스타일
├── index.css                전역 CSS 변수 (미수정)
└── main.jsx                 React entry (미수정)
```

---

## 미구현 / 브로커 팀 연동 후 결정

- [ ] 브로커 상태 카드 — 부하 %, 연결 파라미터 수 (C-02 `_core/sync/load_info`는 Core→Core 전용. 브로커 팀이 client용 토픽으로 노출 시 추가 가능)
- ~~`campus/monitor/ping/+` / `campus/monitor/pong/+` M-01/M-02~~ — 불필요. RTT는 CT `links[].rtt_ms`로 이미 표시됨
