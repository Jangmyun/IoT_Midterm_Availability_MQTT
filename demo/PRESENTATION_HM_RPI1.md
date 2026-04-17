# Presentation Setup: Hyeokmin Mac + Raspberry Pi 1

이 문서는 발표 시연용으로 고정된 Core/Backup 토폴로지를 빠르게 기동하는 방법을 정리한다.

고정 역할:

- Active Core: `192.168.0.7:1883`  (혁민 Mac)
- Backup Core: `192.168.0.8:1883` (라즈베리 1)

관련 스크립트:

- [`demo/presentation-hm-rpi1.sh`](/Users/hyeokkiyaa/Drive/HGU/4th/1st/IoT/MidTermProject/IoT_Midterm_Availability_MQTT/demo/presentation-hm-rpi1.sh:1)

## 사전 준비

각 장비에서 아래 조건이 먼저 만족되어야 한다.

1. 이 저장소가 같은 경로 또는 접근 가능한 경로에 있어야 한다.
2. `broker/build/core_broker`, `broker/build/edge_broker` 가 빌드되어 있어야 한다.
3. 각 장비에서 `mosquitto` 가 해당 장비 IP의 `1883` 포트로 실행 중이어야 한다.

예시:

```bash
cd /Users/hyeokkiyaa/Drive/HGU/4th/1st/IoT/MidTermProject/IoT_Midterm_Availability_MQTT
cmake -S broker -B broker/build
cmake --build broker/build
```

## 한 줄 실행

혁민 Mac (`192.168.0.7`) 에서 Active Core 실행:

```bash
bash demo/presentation-hm-rpi1.sh active-core
```

라즈베리 1 (`192.168.0.8`) 에서 Backup Core 실행:

```bash
bash demo/presentation-hm-rpi1.sh backup-core
```

현재 고정 토폴로지 확인:

```bash
bash demo/presentation-hm-rpi1.sh show
```

## Edge 실행

Edge는 Core/Backup 주소를 스크립트 내부 고정값으로 자동 사용한다.
각 Edge 장비에서는 자기 자신의 로컬 IP와 로컬 MQTT 포트만 넘기면 된다.

예시:

```bash
bash demo/presentation-hm-rpi1.sh edge 192.168.0.9 2883
bash demo/presentation-hm-rpi1.sh edge 192.168.0.10 3883
```

의미:

- `192.168.0.9:2883` 에 로컬 Edge broker가 있다고 가정
- upstream active core는 자동으로 `192.168.0.7:1883`
- upstream backup core는 자동으로 `192.168.0.8:1883`

## 구독 모니터링

Active Core 브로커 구독:

```bash
bash demo/presentation-hm-rpi1.sh active-sub
```

Backup Core 브로커 구독:

```bash
bash demo/presentation-hm-rpi1.sh backup-sub
```

## 추천 시연 순서

1. Mac에서 `mosquitto` 실행
2. 라즈베리 1에서 `mosquitto` 실행
3. Mac에서 `active-core` 실행
4. 라즈베리 1에서 `backup-core` 실행
5. 필요 시 다른 장비에서 `edge ...` 실행
6. Mac 또는 별도 장비에서 `active-sub` 로 토픽 확인

## 빠른 복붙용 명령

Mac:

```bash
cd /Users/hyeokkiyaa/Drive/HGU/4th/1st/IoT/MidTermProject/IoT_Midterm_Availability_MQTT
bash demo/presentation-hm-rpi1.sh active-core
```

라즈베리 1:

```bash
cd /Users/hyeokkiyaa/Drive/HGU/4th/1st/IoT/MidTermProject/IoT_Midterm_Availability_MQTT
bash demo/presentation-hm-rpi1.sh backup-core
```

## 주의

- 이 스크립트는 `core_broker` / `edge_broker` 실행 역할을 고정해주는 용도다.
- `mosquitto` 서비스 자체를 자동으로 띄우지는 않는다.
- 현재 고정 IP는 스크립트에 박혀 있으므로 IP가 바뀌면 [`demo/presentation-hm-rpi1.sh`](/Users/hyeokkiyaa/Drive/HGU/4th/1st/IoT/MidTermProject/IoT_Midterm_Availability_MQTT/demo/presentation-hm-rpi1.sh:1) 상단 값을 수정해야 한다.
