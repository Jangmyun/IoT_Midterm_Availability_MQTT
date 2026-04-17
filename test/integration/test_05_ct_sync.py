"""
TC-CT: Connection Table 동기화 검증
Active Core: 노드 등록 후 _core/sync/connection_table 갱신 발행
Backup Core: Active에서 발행한 CT를 수신
"""
import time
import uuid

import pytest
from conftest import make_publisher, make_status_msg, publish, wait_log, MQTT_HOST


def test_active_publishes_ct_after_node_registration(active_core, spy):
    """
    C-01, FR-01: 노드 등록 후 Active Core가 _core/sync/connection_table(retained)을 갱신 발행한다.

    1. Active Core 기동
    2. Spy가 _core/sync/connection_table 구독
    3. 노드 등록 메시지 발행
    4. Spy가 수신한 CT에 해당 node_id가 포함됨을 확인
    """
    spy.subscribe("_core/sync/connection_table")

    pub = make_publisher()
    try:
        node_id = str(uuid.uuid4())
        reg = make_status_msg(node_id, description=f"{MQTT_HOST}:2883")
        publish(pub, f"campus/monitor/status/{node_id}", reg)

        result = spy.wait_payload(
            "_core/sync/connection_table",
            lambda p: any(n.get("id") == node_id for n in p.get("nodes", [])),
            timeout=10.0,
        )
        assert result is not None, \
            f"CT에 node_id={node_id} 가 없습니다. 수신된 CT: {spy.payloads('_core/sync/connection_table')}"
    finally:
        pub.loop_stop()
        pub.disconnect()


def test_ct_version_increments_on_node_registration(active_core, spy):
    """
    C-01: 노드 등록마다 CT version이 증가한다.
    """
    spy.subscribe("_core/sync/connection_table")

    pub = make_publisher()
    try:
        # 첫 번째 노드 등록
        node_id_1 = str(uuid.uuid4())
        reg1 = make_status_msg(node_id_1, description=f"{MQTT_HOST}:2883")
        publish(pub, f"campus/monitor/status/{node_id_1}", reg1)

        assert spy.wait_for("_core/sync/connection_table", timeout=10.0)
        time.sleep(0.3)

        # 두 번째 노드 등록
        spy.clear()
        node_id_2 = str(uuid.uuid4())
        reg2 = make_status_msg(node_id_2, description=f"{MQTT_HOST}:2884")
        publish(pub, f"campus/monitor/status/{node_id_2}", reg2)

        assert spy.wait_for("_core/sync/connection_table", timeout=10.0)
        ct_payloads = spy.payloads("_core/sync/connection_table")
        latest_version = max(p.get("version", 0) for p in ct_payloads)
        assert latest_version >= 2, \
            f"CT version이 증가하지 않았습니다: version={latest_version}"
    finally:
        pub.loop_stop()
        pub.disconnect()


def test_backup_receives_ct_from_active(active_core, backup_core, spy):
    """
    C-01: Active가 CT를 발행하면 Backup도 같은 retained CT를 수신한다.

    Active와 Backup은 같은 mosquitto에 연결되므로 Backup이 peer 채널로
    _core/sync/connection_table 을 구독하면 retained CT를 즉시 수신한다.
    """
    _, backup_log = backup_core
    spy.subscribe("_core/sync/connection_table")

    pub = make_publisher()
    try:
        # Backup이 peer 브로커에 연결됐는지 확인
        assert wait_log(backup_log, r"\[core/backup\] connected to active broker", timeout=10.0), \
            "Backup Core가 peer 브로커에 연결되지 않았습니다"

        # 노드 등록 → Active CT 갱신
        node_id = str(uuid.uuid4())
        reg = make_status_msg(node_id, description=f"{MQTT_HOST}:2883")
        publish(pub, f"campus/monitor/status/{node_id}", reg)

        # CT가 발행됐는지 확인 (Active → mosquitto retained)
        result = spy.wait_payload(
            "_core/sync/connection_table",
            lambda p: any(n.get("id") == node_id for n in p.get("nodes", [])),
            timeout=10.0,
        )
        assert result is not None, f"CT에 node_id={node_id} 가 없습니다"
    finally:
        pub.loop_stop()
        pub.disconnect()
