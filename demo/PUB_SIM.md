# pub_sim — Publisher Event Simulator 사용 가이드

`pub_sim`은 Core/Edge 성능·가용성 검증을 위한 MQTT 이벤트 발행기다.
실제 Edge 없이 다양한 이벤트를 대량 발행해 Core·Edge 로직을 테스트한다.

빌드 위치: `broker/build/pub_sim`

---

## 빌드

```bash
cmake -S broker -B broker/build
cmake --build broker/build
```

---

## 옵션 전체 목록

| 옵션 | 기본값 | 설명 |
|------|--------|------|
| `--host <addr>` | `localhost` | 브로커 주소 |
| `--port <port>` | `1883` | 브로커 포트 |
| `--id <uuid>` | auto | 고정 publisher UUID |
| `--count <n>` | `10` | 전송 횟수 (`0` = 무제한) |
| `--rate <hz>` | `1` | 초당 이벤트 수 |
| `--events <list>` | `motion,door,intrusion` | 발행할 이벤트 타입 (쉼표 구분) |
| `--building <id>` | `building-a` | payload.building_id |
| `--camera <id>` | `cam-01` | payload.camera_id |
| `--desc <str>` | `sim-pub` | payload.description 태그 |
| `--qos <0\|1\|2>` | `1` | MQTT QoS |
| `--burst` | off | rate 무시, 최대 속도 전송 |
| `--dup [n]` | off / `n=1` | 이벤트마다 n번 중복 재전송 |
| `--register` | off | 루프 전 STATUS 등록 메시지 전송 |
| `--multi-pub` | off | 이벤트마다 새 UUID (다수 Edge 시뮬레이션) |
| `--verbose` | off | 전송 메시지 출력 |

---

## 발행 토픽

```
campus/data/<type>/<building>/<camera>
```

| `--events` 값 | 토픽 예시 |
|---------------|-----------|
| `motion`      | `campus/data/motion/building-a/cam-01` |
| `door`        | `campus/data/door/building-a/cam-01` |
| `intrusion`   | `campus/data/intrusion/building-a/cam-01` |

`--register` 사용 시 추가로 아래 토픽에 STATUS 메시지 1회 발행:
```
campus/monitor/status/<publisher-uuid>
```

---

## 사용 예시

### 1. 기본 동작 확인 (1Hz, 10개)

```bash
./broker/build/pub_sim --host 192.168.0.7
```

### 2. 처리량 측정 — burst 모드

```bash
./broker/build/pub_sim --host 192.168.0.7 --burst --count 5000 --qos 0
```

QoS 0으로 ACK 오버헤드 없이 상한 처리량을 측정한다.
종료 시 `events/sec` 요약이 자동 출력된다.

### 3. 특정 이벤트 타입만 발행

```bash
# intrusion 만
./broker/build/pub_sim --host 192.168.0.7 --events intrusion --count 100 --rate 10

# motion + door 만
./broker/build/pub_sim --host 192.168.0.7 --events motion,door --rate 5 --count 50
```

### 4. Core dedup 검증 — 중복 메시지 주입

Core는 `seen_msg_ids` set으로 중복 메시지를 걸러낸다.
`--dup n`으로 동일 메시지를 n번 추가 전송해 dedup 경로를 부하 테스트한다.

```bash
# 이벤트 1개당 중복 2번 추가 (총 3회 전송)
./broker/build/pub_sim --host 192.168.0.7 --dup 2 --events intrusion --count 100 --rate 20
```

### 5. 다수 Edge 시뮬레이션 — multi-pub

`--multi-pub`은 이벤트마다 새 UUID를 생성해 독립된 Edge 수백 개를 한 프로세스로 시뮬레이션한다.
Core의 CT fan-in 경로와 `seen_msg_ids` set 메모리 사용을 스트레스 테스트한다.

```bash
./broker/build/pub_sim --host 192.168.0.7 --multi-pub --burst --count 2000
```

### 6. Edge 등록 + 데이터 이벤트

`--register`로 Core 등록 경로(CT addNode, topology 재발행)를 먼저 실행한 뒤 이벤트를 발행한다.

```bash
./broker/build/pub_sim --host 192.168.0.7 --register --count 50 --rate 5 --verbose
```

### 7. 고속 연속 발행 (무제한, Ctrl+C 종료)

```bash
./broker/build/pub_sim --host 192.168.0.7 --count 0 --rate 100
```

### 8. Backup Core에 직접 발행

```bash
./broker/build/pub_sim --host 192.168.0.8 --port 1883 --count 200 --rate 20
```

---

## 시연 시나리오별 추천 명령

| 시나리오 | 명령 |
|----------|------|
| 기본 동작 확인 | `pub_sim --host <core-ip> --verbose` |
| 처리량 측정 | `pub_sim --host <core-ip> --burst --count 5000 --qos 0` |
| 우선순위 검증 | `pub_sim --host <core-ip> --events intrusion --count 50 --rate 5 --verbose` |
| dedup 검증 | `pub_sim --host <core-ip> --dup 3 --count 100 --rate 20` |
| 멀티 Edge | `pub_sim --host <core-ip> --multi-pub --burst --count 3000` |
| Edge 등록 포함 | `pub_sim --host <core-ip> --register --count 30 --verbose` |
| Failover 전후 비교 | Core 장애 전후 동일 명령으로 처리량 비교 |

---

## 출력 예시

```
[pub_sim] 브로커 192.168.0.7:1883 연결 완료 (id=3f2a1b...)
[pub] #1 topic=campus/data/motion/building-a/cam-01 msg_id=550e84...
[pub] #2 topic=campus/data/door/building-a/cam-01   msg_id=a1b2c3...
...

[pub_sim] 완료
  전송: 1000 이벤트
  시간: 10023.4 ms
  처리량: 99.8 events/sec
```

---

## 주의

- `pub_sim`은 단방향 발행 전용이다. 토픽 구독·응답 처리는 하지 않는다.
- `--burst` 모드에서 QoS 1/2를 쓰면 브로커 ACK 대기로 실제 처리량이 낮게 측정될 수 있다. 상한 측정은 `--qos 0`을 권장한다.
- `--multi-pub`과 `--register`를 함께 쓰면 등록 메시지는 최초 고정 UUID로만 발행된다.
