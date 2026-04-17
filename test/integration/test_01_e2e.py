"""
TC-E2E: 이벤트 종단간 전달 검증
Publisher → campus/data/INTRUSION → Core 수신·재발행 → Client(Spy) 수신
"""
import pytest
from conftest import make_event, make_publisher, publish, MQTT_HOST, MQTT_PORT


def test_event_delivered_to_client(active_core, edge, spy):
    """
    FR-01: CCTV 이벤트가 Core를 거쳐 클라이언트(구독자)에게 전달된다.

    1. Spy가 campus/data/# 구독
    2. Publisher가 INTRUSION 이벤트 발행
    3. Spy에 해당 msg_id가 있는 이벤트가 수신됨을 확인
    """
    spy.subscribe("campus/data/#")

    pub = make_publisher()
    try:
        event = make_event(event_type="INTRUSION", priority="HIGH")
        msg_id = event["msg_id"]

        publish(pub, "campus/data/INTRUSION", event)

        assert spy.wait_for("campus/data/INTRUSION", timeout=10.0, min_count=1), \
            "campus/data/INTRUSION 토픽에서 메시지를 수신하지 못했습니다"

        payloads = spy.payloads("campus/data/INTRUSION")
        assert any(p.get("msg_id") == msg_id for p in payloads), \
            f"msg_id={msg_id} 를 포함한 페이로드를 찾지 못했습니다. 수신된 ids: {[p.get('msg_id') for p in payloads]}"
    finally:
        pub.loop_stop()
        pub.disconnect()


def test_event_type_preserved(active_core, spy):
    """
    Core 재발행 시 이벤트 type, priority, payload 필드가 보존된다.
    """
    spy.subscribe("campus/data/#")

    pub = make_publisher()
    try:
        event = make_event(
            event_type="MOTION",
            priority="LOW",
            building_id="bldg-b",
            camera_id="cam-03",
        )
        msg_id = event["msg_id"]

        publish(pub, "campus/data/MOTION", event)

        received = spy.wait_payload(
            "campus/data/MOTION",
            lambda p: p.get("msg_id") == msg_id,
            timeout=10.0,
        )
        assert received is not None, "MOTION 이벤트를 수신하지 못했습니다"
        assert received.get("type") == "MOTION"
        assert received.get("priority") == "LOW"
        assert received.get("payload", {}).get("building_id") == "bldg-b"
        assert received.get("payload", {}).get("camera_id") == "cam-03"
    finally:
        pub.loop_stop()
        pub.disconnect()
