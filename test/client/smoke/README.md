# test/client/smoke — 렌더링 스모크 테스트

## 테스트 대상

**웹 클라이언트의 렌더링 로직**

## 목적

각 MQTT 메시지 타입이 도착했을 때 클라이언트 UI에 올바르게 표시되는지 확인합니다.
core/edge가 보내는 메시지를 `mosquitto_pub`으로 흉내내어 브라우저 반응을 수동으로 확인합니다.

## 테스트 케이스

| ID | 스크립트 | 흉내내는 발신자 | MQTT 토픽 | 브라우저에서 확인할 것 |
|---|---|---|---|---|
| PUB-01 | `01_topology.sh` | core | `campus/monitor/topology` (retain) | 토폴로지 그래프·카드 렌더링 |
| PUB-02 | `02_event_intrusion.sh` | edge | `campus/data/INTRUSION` | Event Log에 INTRUSION HIGH 항목 |
| PUB-03 | `03_event_motion.sh` | edge | `campus/data/MOTION/bldg-b` | Event Log에 MOTION 항목 |
| PUB-04 | `04_event_door_forced.sh` | edge | `campus/data/DOOR_FORCED/bldg-c` | Event Log에 DOOR_FORCED HIGH 항목 |
| PUB-05 | `05_node_down.sh` | core | `campus/alert/node_down/<id>` | node_down 배너 + 토폴로지 노드 OFFLINE 갱신 |
| PUB-06 | `06_node_up.sh` | core | `campus/alert/node_up/<id>` | node_up 배너 + 토폴로지 노드 ONLINE 갱신 |
| PUB-07 | `07_core_switch.sh` | core | `campus/alert/core_switch` | core_switch 배너 표시 |
| PUB-08 | `08_core_lwt.sh` | core (LWT) | `campus/will/core/<id>` | core LWT 배너 표시 |

> **PUB-05 / PUB-06 페이로드**: `core/main.cpp`가 실제로 보내는 것과 동일하게 **ConnectionTable JSON**을 사용합니다.
> 클라이언트는 이를 파싱해 topology를 갱신하고 버전 기반 중복 제거를 적용합니다.

## 실행

```bash
# 개별
./test/test_pub.sh node_down
./test/test_pub.sh node_up

# 전체 (PUB-01~08 순서대로)
./test/test_pub.sh all_client
```
