# test/client/edge-cases — 파싱·방어 로직 테스트

## 테스트 대상

**웹 클라이언트의 방어 로직** (`useMqtt.js`, `parsers.js`)

## 목적

비정상적이거나 경계 조건의 입력에 대해 클라이언트가 올바르게 처리하는지 확인합니다.
렌더링이 아닌 **드롭·무시·중복 제거** 동작이 핵심입니다.

## 테스트 케이스

| ID | 스크립트 | 검증 내용 | 기대 동작 |
|---|---|---|---|
| EC-01 | `01_duplicate_event.sh` | 같은 `msg_id` 이벤트 2회 publish | `seenMsgIds` 기반 중복 제거 → Event Log에 1건만 표시 |
| EC-02 | `02_stale_topology.sh` | topology v10 → v9 순서로 publish | version guard → 낮은 버전(v9) 무시, UI 버전 그대로 |
| EC-03 | `03_invalid_event_uuid.sh` | `msg_id`가 UUID 형식이 아닌 이벤트 | `parseMqttMessage` 파서가 null 반환 → Event Log에 추가되지 않음 |
| EC-04 | `04_missing_priority.sh` | `priority` 필드가 없는 이벤트 | 정상 수신, priority는 `null`로 처리 → HIGH 배지 없음 |

## 중복 제거 메커니즘 (클라이언트 구현)

| 데이터 종류 | 키 | 위치 |
|---|---|---|
| CCTV 이벤트 (`campus/data/`) | `msg_id` UUID | `seenMsgIds` Set |
| node_down / node_up 알림 | `topic:ct.version` | `seenAlertKeys` Set |
| core_switch / will/core | `msg_id` UUID | `seenMsgIds` Set |
| topology | `version` 숫자 비교 | version guard (`<=` 무시) |

## 실행

```bash
./test/test_pub.sh duplicate_event
./test/test_pub.sh stale_topology
./test/test_pub.sh invalid_uuid
./test/test_pub.sh missing_priority
```
