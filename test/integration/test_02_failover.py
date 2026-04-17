"""
TC-FAILOVER: Active Core 장애 → Backup 자동 인계 검증
Active SIGKILL → LWT → Backup이 campus/alert/core_switch 발행 → Edge 재연결
"""
import signal
import time

import pytest
from conftest import wait_log, MQTT_HOST, MQTT_PORT


def test_backup_publishes_core_switch_on_active_failure(active_core, backup_core, spy):
    """
    FR-04, FR-05, A-03: Active Core 강제 종료 시 Backup이 campus/alert/core_switch 발행.

    1. Active + Backup 기동
    2. Active SIGKILL
    3. Spy가 campus/alert/core_switch 수신 확인
    """
    spy.subscribe("campus/alert/core_switch")

    active_proc, _ = active_core
    # Backup이 peer 연결을 완료했는지 확인
    _, backup_log = backup_core
    assert wait_log(backup_log, r"\[core/backup\] connected to active broker", timeout=10.0), \
        "Backup Core가 Active peer 브로커에 연결되지 않았습니다"

    # Active 강제 종료 (LWT 트리거)
    active_proc.kill()
    active_proc.wait(timeout=3)

    assert spy.wait_for("campus/alert/core_switch", timeout=15.0), \
        "campus/alert/core_switch 가 발행되지 않았습니다"

    payloads = spy.payloads("campus/alert/core_switch")
    assert len(payloads) >= 1
    # payload.description 에 새 Core(Backup)의 IP 포함 여부 확인
    assert any(
        MQTT_HOST in str(p.get("payload", {}).get("description", ""))
        for p in payloads
    ), f"core_switch payload에 IP({MQTT_HOST})가 없습니다: {payloads}"


def test_edge_reconnects_after_core_switch(active_core, backup_core, edge, spy):
    """
    FR-05: Edge가 campus/alert/core_switch 수신 후 새 Core 주소로 재연결 시도한다.

    1. Active + Backup + Edge 기동
    2. Active SIGKILL → Backup이 core_switch 발행
    3. Edge 로그에 재연결 시도 로그 확인
    """
    spy.subscribe("campus/alert/core_switch")

    active_proc, _ = active_core
    _, backup_log = backup_core
    _, edge_log = edge

    assert wait_log(backup_log, r"\[core/backup\] connected to active broker", timeout=10.0), \
        "Backup Core가 peer 브로커에 연결되지 않았습니다"

    active_proc.kill()
    active_proc.wait(timeout=3)

    # core_switch 발행 대기
    assert spy.wait_for("campus/alert/core_switch", timeout=15.0), \
        "campus/alert/core_switch 가 발행되지 않았습니다"

    # Edge가 core_switch 수신 후 처리 로그 확인
    assert wait_log(edge_log, r"core_switch: new active core at", timeout=10.0), \
        "Edge가 campus/alert/core_switch 를 수신하지 못했습니다"
