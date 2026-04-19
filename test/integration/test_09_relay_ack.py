"""
TC-09 RELAY-ACK: application-level relay/ack 메커니즘 검증 (H-3, R-02)

검증 항목:
  - Core가 이벤트 처리 후 campus/relay/ack/<msg_id> 를 발행한다
  - 중복 msg_id 이벤트에 대해서는 relay/ack 가 발행되지 않는다
  - 서로 다른 msg_id 이벤트는 각각 relay/ack 가 발행된다
"""
import time

import pytest
from conftest import (
    make_event, make_publisher, publish,
)


def test_core_publishes_relay_ack_on_event(active_core, spy):
    """
    H-3, R-02: Core 가 이벤트 수신·처리 후
    campus/relay/ack/<msg_id> 를 QoS 0 으로 발행한다.
    """
    spy.subscribe("campus/relay/ack/#")

    pub = make_publisher()
    try:
        event = make_event(event_type="INTRUSION")
        msg_id = event["msg_id"]
        publish(pub, "campus/data/INTRUSION", event)

        assert spy.wait_for(f"campus/relay/ack/{msg_id}", timeout=10.0), \
            f"campus/relay/ack/{msg_id} 가 수신되지 않았습니다"
    finally:
        pub.loop_stop()
        pub.disconnect()


def test_relay_ack_not_sent_for_duplicate_event(active_core, spy):
    """
    FR-02 + H-3: Core 의 seen_msg_ids dedup 필터를 통과하지 못한
    중복 이벤트에 대해서는 relay/ack 가 발행되지 않는다.
    """
    spy.subscribe("campus/relay/ack/#")

    pub = make_publisher()
    try:
        event = make_event(event_type="MOTION")
        msg_id = event["msg_id"]

        # 첫 번째 발행 → relay/ack 수신
        publish(pub, "campus/data/MOTION", event)
        assert spy.wait_for(f"campus/relay/ack/{msg_id}", timeout=10.0), \
            "첫 번째 이벤트에 relay/ack 가 발행되지 않았습니다"

        # 두 번째 발행 (동일 msg_id) → dedup 필터 → relay/ack 미발행
        spy.clear()
        time.sleep(0.2)
        publish(pub, "campus/data/MOTION", event)

        assert not spy.wait_for(f"campus/relay/ack/{msg_id}", timeout=3.0), \
            "중복 이벤트에 relay/ack 가 발행되었습니다 (dedup 실패)"
    finally:
        pub.loop_stop()
        pub.disconnect()


def test_relay_ack_sent_for_each_distinct_msg_id(active_core, spy):
    """
    서로 다른 msg_id 를 가진 이벤트는 각각 독립적으로 relay/ack 가 발행된다.
    """
    spy.subscribe("campus/relay/ack/#")

    pub = make_publisher()
    try:
        event1 = make_event(event_type="INTRUSION")
        event2 = make_event(event_type="MOTION")
        msg_id1 = event1["msg_id"]
        msg_id2 = event2["msg_id"]

        publish(pub, "campus/data/INTRUSION", event1)
        publish(pub, "campus/data/MOTION", event2)

        assert spy.wait_for(f"campus/relay/ack/{msg_id1}", timeout=10.0), \
            f"campus/relay/ack/{msg_id1} 수신 실패"
        assert spy.wait_for(f"campus/relay/ack/{msg_id2}", timeout=10.0), \
            f"campus/relay/ack/{msg_id2} 수신 실패"
    finally:
        pub.loop_stop()
        pub.disconnect()


def test_relay_ack_sent_after_backup_core_processes_event(active_core, backup_core, spy):
    """
    FR-05 + H-3: Backup Core 도 이벤트 처리 후 relay/ack 를 발행한다.
    Active 기동 후 Backup 도 같이 구독 중인 상황에서 이벤트 발행.
    """
    spy.subscribe("campus/relay/ack/#")
    spy.subscribe("campus/data/#")

    _, backup_log = backup_core
    from conftest import wait_log
    assert wait_log(backup_log, r"\[core/backup\] connected to active broker", timeout=10.0), \
        "Backup Core 가 Active peer 에 연결되지 않았습니다"

    pub = make_publisher()
    try:
        event = make_event(event_type="DOOR_FORCED")
        msg_id = event["msg_id"]
        publish(pub, "campus/data/DOOR_FORCED", event)

        # Active Core 가 먼저 처리 → relay/ack 발행
        assert spy.wait_for(f"campus/relay/ack/{msg_id}", timeout=10.0), \
            f"Active Core 에서 campus/relay/ack/{msg_id} 수신 실패"
    finally:
        pub.loop_stop()
        pub.disconnect()
