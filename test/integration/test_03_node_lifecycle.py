"""
TC-NODE: Node 장애(LWT) 감지 및 복구 검증
등록 → LWT → campus/alert/node_down → 재등록 → campus/alert/node_up
"""
import time
import uuid

import pytest
from conftest import make_publisher, make_status_msg, publish, MQTT_HOST


def test_node_offline_on_lwt(active_core, spy):
    """
    FR-06, A-01: Node LWT 수신 → Core가 campus/alert/node_down/{id} 발행 (OFFLINE 포함).

    1. Core 기동
    2. 노드 등록 (STATUS, description="ip:port")
    3. 노드 LWT 시뮬레이션 (STATUS, description="" → campus/will/node/{id})
    4. campus/alert/node_down/{id} 에 OFFLINE 페이로드 수신 확인
    """
    node_id = str(uuid.uuid4())
    spy.subscribe(f"campus/alert/node_down/{node_id}")

    pub = make_publisher()
    try:
        # 노드 등록
        reg = make_status_msg(node_id, description=f"{MQTT_HOST}:2883")
        publish(pub, f"campus/monitor/status/{node_id}", reg)
        time.sleep(0.5)

        # LWT 시뮬레이션
        lwt = make_status_msg(node_id, description="")
        publish(pub, f"campus/will/node/{node_id}", lwt)

        down = spy.wait_payload(
            f"campus/alert/node_down/{node_id}",
            lambda p: p.get("payload", {}).get("description", "").upper() == "OFFLINE"
                      or "OFFLINE" in str(p),
            timeout=10.0,
        )
        assert down is not None, \
            f"campus/alert/node_down/{node_id} 에 OFFLINE 페이로드를 수신하지 못했습니다"
    finally:
        pub.loop_stop()
        pub.disconnect()


def test_node_recovery_after_lwt(active_core, spy):
    """
    FR-13, A-02: Node 재등록 → Core가 campus/alert/node_up/{id} 발행 (ONLINE 포함).

    1. 노드 등록 → LWT(OFFLINE) → node_down 확인
    2. 재등록 → node_up 확인
    """
    node_id = str(uuid.uuid4())
    spy.subscribe(
        f"campus/alert/node_down/{node_id}",
        f"campus/alert/node_up/{node_id}",
    )

    pub = make_publisher()
    try:
        reg = make_status_msg(node_id, description=f"{MQTT_HOST}:1883")
        lwt = make_status_msg(node_id, description="")

        # 등록 → LWT
        publish(pub, f"campus/monitor/status/{node_id}", reg)
        time.sleep(0.5)
        publish(pub, f"campus/will/node/{node_id}", lwt)

        assert spy.wait_for(f"campus/alert/node_down/{node_id}", timeout=10.0), \
            "node_down alert 수신 실패"

        # 재등록
        time.sleep(0.3)
        reg2 = make_status_msg(node_id, description=f"{MQTT_HOST}:1883")
        publish(pub, f"campus/monitor/status/{node_id}", reg2)

        up = spy.wait_payload(
            f"campus/alert/node_up/{node_id}",
            lambda p: "ONLINE" in str(p),
            timeout=10.0,
        )
        assert up is not None, \
            f"campus/alert/node_up/{node_id} 에 ONLINE 페이로드를 수신하지 못했습니다"
    finally:
        pub.loop_stop()
        pub.disconnect()
