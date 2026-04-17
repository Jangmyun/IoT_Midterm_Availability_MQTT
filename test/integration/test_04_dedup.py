"""
TC-DEDUP: 이벤트 중복 발행 방지 검증
동일 msg_id 이벤트 2회 발행 → Core가 1회만 재발행
"""
import time

import pytest
from conftest import make_event, make_publisher, publish


def test_duplicate_event_forwarded_once(active_core, spy):
    """
    FR-02: 동일 msg_id 이벤트를 연속 2회 발행해도 Core는 1회만 재발행한다.

    검증 방법: Core 프로세스 로그에서 "event forwarded" 횟수가 정확히 1인지 확인.
    """
    _, core_log = active_core
    spy.subscribe("campus/data/#")

    pub = make_publisher()
    try:
        event = make_event(event_type="MOTION", priority="LOW")

        # 첫 번째 발행
        publish(pub, "campus/data/MOTION", event)

        # Core가 첫 이벤트를 수신·재발행할 때까지 대기
        assert spy.wait_for("campus/data/MOTION", timeout=10.0, min_count=1), \
            "첫 번째 이벤트가 Core를 거쳐 전달되지 않았습니다"

        # 동일 msg_id로 재발행
        time.sleep(0.5)
        publish(pub, "campus/data/MOTION", event)

        # 충분히 기다린 후 로그 확인
        time.sleep(2.0)

        log_content = core_log.read_text(errors="replace")
        forward_count = log_content.count("event forwarded: campus/data/MOTION")
        assert forward_count == 1, \
            f"Core가 재발행한 횟수: {forward_count} (기대: 1)"
    finally:
        pub.loop_stop()
        pub.disconnect()


def test_different_msg_ids_both_forwarded(active_core, spy):
    """
    FR-02 보완: msg_id가 다른 두 이벤트는 각각 재발행된다 (dedup이 과하게 적용되지 않음).
    """
    _, core_log = active_core
    spy.subscribe("campus/data/#")

    pub = make_publisher()
    try:
        event_a = make_event(event_type="INTRUSION")
        event_b = make_event(event_type="INTRUSION")  # 새 msg_id 자동 생성

        publish(pub, "campus/data/INTRUSION", event_a)
        time.sleep(0.3)
        publish(pub, "campus/data/INTRUSION", event_b)

        time.sleep(2.0)

        log_content = core_log.read_text(errors="replace")
        forward_count = log_content.count("event forwarded: campus/data/INTRUSION")
        assert forward_count >= 2, \
            f"두 이벤트가 각각 재발행돼야 하는데 forward 횟수={forward_count}"
    finally:
        pub.loop_stop()
        pub.disconnect()
