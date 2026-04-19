"""
TC-PEER-RELAY: EdgeC(peer-only) → EdgeA(direct) → Core 이벤트 전달 검증

토폴로지:
  Core (MQTT_PORT)
    ↑
  EdgeA (local_broker: LOCAL_PORT_A) → upstream CORE → Core
    ↑
  EdgeC (local_broker: LOCAL_PORT_C) → upstream PEER_EDGE → EdgeA local_broker

검증 항목:
  1. EdgeC가 EdgeA 로컬 브로커를 통해 CT를 수신 (CT applied)
  2. EdgeA가 EdgeC status를 Core로 전달 (forwarded peer status)
  3. EdgeC 이벤트가 EdgeA를 경유하여 Core에 도달
  4. RTT 이후 EdgeA가 EdgeC에 peer 연결 시도 (동적 peer 연결)
  5. EdgeA 일시 중단 시 EdgeC가 이벤트를 큐잉 후 재전송 (store-and-forward)
"""
import os
import signal
import subprocess
import time

import paho.mqtt.client as mqtt
import pytest

from conftest import (
    EDGE_BINARY,
    MQTT_HOST,
    MQTT_PORT,
    make_publisher,
    run_proc,
    wait_log,
)

# ── 포트 설정 ─────────────────────────────────────────────────────────────────
LOCAL_PORT_A = int(os.environ.get("EDGE_A_LOCAL_PORT", "12883"))
LOCAL_PORT_C = int(os.environ.get("EDGE_C_LOCAL_PORT", "12884"))


# ── 로컬 브로커 픽스처 ──────────────────────────────────────────────────────────
@pytest.fixture
def local_broker_a(tmp_path):
    """EdgeA 전용 로컬 mosquitto 브로커 (LOCAL_PORT_A)"""
    log = tmp_path / "broker_a.log"
    proc = subprocess.Popen(
        ["mosquitto", "-p", str(LOCAL_PORT_A)],
        stdout=open(log, "w"),
        stderr=subprocess.STDOUT,
    )
    time.sleep(0.5)
    yield LOCAL_PORT_A
    proc.terminate()
    proc.wait()


@pytest.fixture
def local_broker_c(tmp_path):
    """EdgeC 전용 로컬 mosquitto 브로커 (LOCAL_PORT_C)"""
    log = tmp_path / "broker_c.log"
    proc = subprocess.Popen(
        ["mosquitto", "-p", str(LOCAL_PORT_C)],
        stdout=open(log, "w"),
        stderr=subprocess.STDOUT,
    )
    time.sleep(0.5)
    yield LOCAL_PORT_C
    proc.terminate()
    proc.wait()


# ── Edge 픽스처 ────────────────────────────────────────────────────────────────
@pytest.fixture
def edge_a(tmp_path, active_core, local_broker_a):
    """EdgeA: Core에 직접 연결 (local=local_broker_a, upstream=Core)"""
    log = tmp_path / "edge_a.log"
    os.environ["EDGE_ID_SUFFIX"] = "-peerA"
    with run_proc(
        EDGE_BINARY,
        MQTT_HOST, str(LOCAL_PORT_A),    # local broker
        MQTT_HOST, str(MQTT_PORT),       # upstream core
        startup_log_pattern=r"\[edge\] registered to core",
        startup_timeout=15.0,
        log_path=log,
    ) as proc:
        os.environ.pop("EDGE_ID_SUFFIX", None)
        # EdgeA가 CT를 받아 로컬 브로커에 retain 재발행할 시간 대기
        time.sleep(1.0)
        yield proc, log


@pytest.fixture
def edge_c(tmp_path, active_core, edge_a, local_broker_a, local_broker_c):
    """EdgeC: peer-only (local=local_broker_c, upstream PEER_EDGE=EdgeA local_broker)"""
    log = tmp_path / "edge_c.log"
    os.environ["EDGE_PEER_ONLY"] = "1"
    os.environ["EDGE_UPSTREAM_PEERS"] = f"127.0.0.1:{LOCAL_PORT_A}"
    os.environ["EDGE_ID_SUFFIX"] = "-peerC"
    with run_proc(
        EDGE_BINARY,
        MQTT_HOST, str(LOCAL_PORT_C),    # local broker
        MQTT_HOST, str(MQTT_PORT),       # used only for outbound IP detection
        startup_log_pattern=r"\[edge\] connected to peer edge",
        startup_timeout=15.0,
        log_path=log,
    ) as proc:
        os.environ.pop("EDGE_PEER_ONLY", None)
        os.environ.pop("EDGE_UPSTREAM_PEERS", None)
        os.environ.pop("EDGE_ID_SUFFIX", None)
        yield proc, log


# ── TC-01: EdgeC가 CT를 peer 경유로 수신 ─────────────────────────────────────
def test_edge_c_receives_ct_via_peer(active_core, edge_a, edge_c):
    """
    FR-08: EdgeC가 EdgeA 로컬 브로커에 구독하여 retained CT를 수신한다.
    EdgeA는 Core로부터 CT를 받은 뒤 로컬 브로커에 retain 재발행한다.
    """
    _, log_c = edge_c
    assert wait_log(log_c, r"CT applied", timeout=15.0), \
        "EdgeC가 peer 경유로 CT를 수신하지 못했습니다"


# ── TC-02: EdgeA가 EdgeC status를 Core로 전달 ─────────────────────────────────
def test_peer_status_forwarded_to_core(active_core, edge_a, edge_c):
    """
    EdgeC가 EdgeA 로컬 브로커에 status를 발행하면
    EdgeA가 campus/monitor/status/# 구독으로 이를 수신하여 Core로 전달한다.
    """
    _, log_a = edge_a
    assert wait_log(log_a, r"forwarded peer status", timeout=15.0), \
        "EdgeA가 EdgeC의 status를 Core로 전달하지 않았습니다"


# ── TC-03: EdgeC 이벤트가 EdgeA를 경유하여 Core에 도달 ────────────────────────
def test_edge_c_event_reaches_core(active_core, edge_a, edge_c, spy):
    """
    EdgeC 로컬 브로커에 발행된 이벤트가
    EdgeC → (PEER_EDGE upstream) → EdgeA 로컬 브로커 → EdgeA → Core 경로로 전달된다.
    """
    spy.subscribe("campus/data/#")
    time.sleep(0.3)

    pub = make_publisher(host=MQTT_HOST, port=LOCAL_PORT_C)
    pub.publish(
        "campus/data/motion/B1/CAM-PEER",
        '{"type":"MOTION","priority":"LOW"}',
        qos=1,
    )
    time.sleep(0.1)
    pub.loop_stop()
    pub.disconnect()

    assert spy.wait_for("campus/data/#", timeout=15.0), \
        "EdgeC 이벤트가 Core 브로커에 도달하지 않았습니다"


# ── TC-04: RTT 이후 EdgeA가 EdgeC에 peer 연결 시도 ───────────────────────────
def test_dynamic_peer_connection_after_rtt(active_core, edge_a, edge_c):
    """
    EdgeA가 Core로부터 CT를 받아 EdgeC의 정보를 확인한 뒤
    Ping/Pong RTT 계산 완료 후 EdgeC에 동적 peer 연결을 시도한다.
    """
    _, log_a = edge_a
    assert wait_log(log_a, r"initiating peer connection to", timeout=20.0), \
        "EdgeA가 EdgeC에 peer 연결을 시도하지 않았습니다"


# ── TC-05: EdgeA 중단 시 EdgeC store-and-forward ──────────────────────────────
def test_store_and_forward_when_peer_down(active_core, edge_a, edge_c, spy):
    """
    EdgeA 프로세스를 일시 중단(SIGSTOP)하면 EdgeC가 이벤트를 큐에 저장하고,
    EdgeA 재개(SIGCONT) 후 큐의 이벤트가 Core로 전달된다.
    """
    proc_a, _ = edge_a
    _, log_c = edge_c

    # CT 수신 완료 후 시작
    assert wait_log(log_c, r"CT applied", timeout=15.0), \
        "사전 조건: EdgeC가 CT를 수신하지 못했습니다"

    spy.subscribe("campus/data/#")

    # EdgeA 일시 중단 → EdgeC upstream 단절
    proc_a.send_signal(signal.SIGSTOP)
    time.sleep(0.5)

    # EdgeC 로컬 브로커에 이벤트 발행 (upstream이 끊겨 큐잉되어야 함)
    pub = make_publisher(host=MQTT_HOST, port=LOCAL_PORT_C)
    pub.publish(
        "campus/data/motion/B1/CAM-QUEUED",
        '{"type":"MOTION","priority":"LOW"}',
        qos=1,
    )
    time.sleep(0.3)
    pub.loop_stop()
    pub.disconnect()

    assert wait_log(log_c, r"queued event for later delivery", timeout=5.0), \
        "EdgeC가 이벤트를 큐에 저장하지 않았습니다"

    # EdgeA 재개 → 연결 복구 후 flush
    proc_a.send_signal(signal.SIGCONT)

    assert spy.wait_for("campus/data/#", timeout=15.0), \
        "EdgeA 재개 후 큐잉된 이벤트가 Core에 도달하지 않았습니다"
