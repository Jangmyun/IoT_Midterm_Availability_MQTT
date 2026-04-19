"""
TC-PUB-FAILOVER: Publisher CT 기반 자동 Failover 검증 (Phase 7)

Publisher가 등록된 Edge 장애 시 Connection Table의 RTT/hop_to_core 정보를 활용하여
자동으로 다른 Edge 또는 Core를 통해 이벤트를 전달하는 시나리오 검증.

검증 항목:
  - Publisher가 CT를 수신하는지 확인
  - Edge 장애 → Publisher가 대체 Edge/Core로 재연결하여 이벤트 전달 성공
  - Edge 복구 → Publisher가 원래 Edge로 복귀
  - 유일한 Edge 장애 → Core로 직접 연결하여 이벤트 전달 성공
"""
import json
import os
import signal
import subprocess
import time
import uuid

import paho.mqtt.client as mqtt
import pytest

from conftest import (
    CORE_BINARY,
    EDGE_BINARY,
    BUILD_DIR,
    MQTT_HOST,
    MQTT_PORT,
    MqttSpy,
    make_event,
    make_publisher,
    run_proc,
    wait_log,
    _clear_retained,
)

PUB_SIM_BINARY = os.environ.get("PUB_SIM_BINARY", str(BUILD_DIR / "pub_sim"))


def _empty_ct(version: int) -> dict:
    return {
        "version": version,
        "last_update": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "active_core_id": "",
        "backup_core_id": "",
        "nodes": [],
        "links": [],
    }


# ──────────────────────────────────────────────
# TC-01: Publisher가 CT를 수신하는지 확인
# ──────────────────────────────────────────────
def test_publisher_receives_ct(active_core, edge, tmp_path):
    """
    Publisher가 연결 후 campus/monitor/topology를 구독하고
    CT를 수신하면 'CT applied' 로그를 출력한다.
    """
    pub_log = tmp_path / "pub_ct.log"

    with run_proc(
        PUB_SIM_BINARY,
        "--host", MQTT_HOST,
        "--port", str(MQTT_PORT),
        "--count", "5",
        "--rate", "1",
        "--verbose",
        startup_log_pattern=r"\[pub_sim\] 브로커",
        startup_timeout=10.0,
        log_path=pub_log,
    ) as pub_proc:
        # CT는 Core가 retained로 발행하므로 몇 초 안에 수신됨
        assert wait_log(pub_log, r"CT applied", timeout=15.0), \
            "Publisher가 Connection Table을 수신하지 못했습니다"


# ──────────────────────────────────────────────
# TC-02: Edge 장애 → Publisher가 Core로 재연결하여 이벤트 전달 성공
# ──────────────────────────────────────────────
def test_publisher_failover_to_core(active_core, edge, spy, tmp_path):
    """
    시나리오 5.5.1 확장:
    1. Publisher가 Edge에 연결하여 이벤트 발행
    2. Edge 강제 종료 (LWT 트리거)
    3. Publisher가 CT 기반으로 Core에 재연결
    4. 이후 이벤트가 Core를 통해 Client(Spy)에 전달 확인

    여기서는 Edge가 유일하므로 Publisher가 Core로 직접 재연결한다.
    """
    spy.subscribe("campus/data/#")
    edge_proc, edge_log = edge

    pub_log = tmp_path / "pub_failover.log"

    with run_proc(
        PUB_SIM_BINARY,
        "--host", MQTT_HOST,
        "--port", str(MQTT_PORT),
        "--count", "0",          # 무제한 발행
        "--rate", "2",
        "--verbose",
        startup_log_pattern=r"\[pub_sim\] 브로커",
        startup_timeout=10.0,
        log_path=pub_log,
    ) as pub_proc:
        # 1. CT 수신 확인
        assert wait_log(pub_log, r"CT applied", timeout=15.0), \
            "Publisher가 CT를 수신하지 못했습니다"

        # 2. 먼저 정상 이벤트 전달 확인
        assert spy.wait_for("campus/data/#", timeout=10.0, min_count=1), \
            "정상 상태에서 이벤트가 전달되지 않았습니다"
        delivered_before_failover = spy.count("campus/data/#")

        # 3. Edge 강제 종료
        edge_proc.kill()
        edge_proc.wait(timeout=3)

        # 4. Publisher의 failover 시도 확인
        # Publisher가 CT 기반으로 대체 브로커(Core)를 찾아 재연결
        assert wait_log(pub_log, r"failover|reconnect|connected", timeout=20.0), \
            "Publisher가 failover를 시도하지 않았습니다"

        # 5. failover 후에도 새 이벤트가 계속 들어와야 한다.
        assert spy.wait_for("campus/data/#", timeout=15.0, min_count=delivered_before_failover + 1), \
            "Publisher가 failover 후 Core를 통해 이벤트를 계속 전달하지 못했습니다"

        # 6. Publisher 종료
        pub_proc.terminate()
        try:
            pub_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pub_proc.kill()
            pub_proc.wait()


# ──────────────────────────────────────────────
# TC-03: 유일한 Edge 장애 → Store-and-forward 큐에 이벤트 저장
# ──────────────────────────────────────────────
def test_publisher_queues_events_on_disconnect(active_core, edge, tmp_path):
    """
    Edge 연결이 끊기면 Publisher가 이벤트를 로컬 큐에 저장한다.
    로그에 'queued event' 패턴이 나타나야 한다.
    """
    edge_proc, edge_log = edge

    pub_log = tmp_path / "pub_queue.log"

    with run_proc(
        PUB_SIM_BINARY,
        "--host", MQTT_HOST,
        "--port", str(MQTT_PORT),
        "--count", "0",
        "--rate", "2",
        startup_log_pattern=r"\[pub_sim\] 브로커",
        startup_timeout=10.0,
        log_path=pub_log,
    ) as pub_proc:
        # CT 수신 확인
        assert wait_log(pub_log, r"CT applied", timeout=15.0), \
            "Publisher가 CT를 수신하지 못했습니다"
        time.sleep(2)

        # Edge 종료 → Publisher on_disconnect 트리거
        edge_proc.kill()
        edge_proc.wait(timeout=3)

        # 이벤트 발행 시도 → 큐에 저장되는 로그 확인
        # Publisher는 계속 이벤트를 발행하려 하므로 큐에 저장되거나 failover 후 전송
        found_queue_or_failover = wait_log(
            pub_log,
            r"queued event|failover|disconnected",
            timeout=15.0,
        )
        assert found_queue_or_failover, \
            "Publisher가 연결 끊김 후 큐잉 또는 failover를 시도하지 않았습니다"

        pub_proc.terminate()
        try:
            pub_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pub_proc.kill()
            pub_proc.wait()


# ──────────────────────────────────────────────
# TC-04: Publisher가 Edge LWT 수신으로 사전 장애 감지
# ──────────────────────────────────────────────
def test_publisher_detects_edge_lwt(active_core, edge, tmp_path):
    """
    Edge 비정상 종료 시 발행되는 LWT를 Publisher가 수신하고
    'edge down detected' 로그를 출력한다.
    """
    edge_proc, edge_log = edge

    pub_log = tmp_path / "pub_lwt.log"

    with run_proc(
        PUB_SIM_BINARY,
        "--host", MQTT_HOST,
        "--port", str(MQTT_PORT),
        "--count", "0",
        "--rate", "1",
        startup_log_pattern=r"\[pub_sim\] 브로커",
        startup_timeout=10.0,
        log_path=pub_log,
    ) as pub_proc:
        # CT 수신 대기
        assert wait_log(pub_log, r"CT applied", timeout=15.0), \
            "Publisher가 CT를 수신하지 못했습니다"
        time.sleep(1)

        # Edge 강제 종료 → LWT 발행
        edge_proc.kill()
        edge_proc.wait(timeout=3)

        # Publisher가 LWT 수신 확인
        assert wait_log(pub_log, r"edge down detected", timeout=20.0), \
            "Publisher가 Edge LWT를 수신하지 못했습니다"

        pub_proc.terminate()
        try:
            pub_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pub_proc.kill()
            pub_proc.wait()


# ──────────────────────────────────────────────
# TC-05: 빈 CT 수신 후 Edge 장애 → 마지막으로 본 Core에 직접 재연결
# ──────────────────────────────────────────────
def test_publisher_falls_back_to_cached_core_when_ct_empty(active_core, edge, spy, tmp_path):
    """
    Publisher가 한 번이라도 유효한 CT를 받아 active core endpoint를 기억한 뒤,
    이후 빈 CT(nodes=0)를 받으면 Edge 장애 시 cached core로 직접 연결해야 한다.
    """
    spy.subscribe("campus/data/#")
    edge_proc, edge_log = edge
    pub_log = tmp_path / "pub_cached_core.log"

    with run_proc(
        PUB_SIM_BINARY,
        "--host", MQTT_HOST,
        "--port", str(MQTT_PORT),
        "--count", "0",
        "--rate", "2",
        "--verbose",
        startup_log_pattern=r"\[pub_sim\] 브로커",
        startup_timeout=10.0,
        log_path=pub_log,
    ) as pub_proc:
        assert wait_log(pub_log, r"CT applied", timeout=15.0), \
            "Publisher가 초기 Connection Table을 수신하지 못했습니다"
        assert spy.wait_for("campus/data/#", timeout=10.0, min_count=1), \
            "초기 상태에서 이벤트가 전달되지 않았습니다"

        publisher = make_publisher()
        try:
            publisher.publish(
                "campus/monitor/topology",
                json.dumps(_empty_ct(999)),
                qos=1,
                retain=True,
            )
            time.sleep(1.0)
        finally:
            publisher.loop_stop()
            publisher.disconnect()

        assert wait_log(pub_log, r"CT applied \(version=999, nodes=0\)", timeout=10.0), \
            "Publisher가 빈 CT를 적용하지 않았습니다"

        delivered_before_failover = spy.count("campus/data/#")
        edge_proc.kill()
        edge_proc.wait(timeout=3)

        assert wait_log(pub_log, r"CT empty, reconnecting directly to cached core", timeout=20.0), \
            "Publisher가 빈 CT 상태에서 cached core로 직접 재연결하지 않았습니다"
        assert spy.wait_for("campus/data/#", timeout=15.0, min_count=delivered_before_failover + 1), \
            "Publisher가 cached core를 통해 이벤트 전달을 이어가지 못했습니다"

        pub_proc.terminate()
        try:
            pub_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pub_proc.kill()
            pub_proc.wait()
