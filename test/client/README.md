# test/client — 웹 클라이언트 테스트

## 테스트 대상

**웹 클라이언트** (`client/src/`)

브로커에서 MQTT 메시지가 도착했을 때 클라이언트가 올바르게 동작하는지 검증합니다.

## 핵심 원칙

> 이 폴더의 테스트는 **클라이언트가 테스트 대상**입니다.
> `mosquitto_pub`로 core/edge가 보내는 메시지를 흉내내어 클라이언트의 반응을 브라우저에서 확인합니다.
> 실제 바이너리 없이 `mosquitto_pub`만으로 실행 가능합니다.

## 하위 폴더

| 폴더 | 목적 |
|---|---|
| `smoke/` | 각 메시지 타입이 UI에 정상 렌더링되는지 확인 |
| `edge-cases/` | 비정상 입력에 대한 방어 로직(중복 제거, 버전 가드, 파서 드롭) 확인 |

## 실행

```bash
# 전체 실행
./test/test_pub.sh all_client

# 개별 실행
./test/test_pub.sh node_down
./test/test_pub.sh duplicate_event
```

## 사전 조건

- mosquitto 브로커 실행 중 (`mosquitto -c mosquitto.conf`)
- 브라우저에서 클라이언트 열기 (`cd client && npm run dev` → `http://localhost:5173`)
