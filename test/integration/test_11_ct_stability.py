"""
TC-11 CT_STABILITY: CT 적용 안정성 검증

검증 항목:
  TC-11-01: 동일 버전 CT 가 두 번 이상 적용되지 않는다
            (ct_mutex race condition fix — Core + Backup 이 동시에 같은 버전 전송)
  TC-11-02: RTT 보고 후 추가 ping 이 발생하지 않는다
            (structural_change guard — RTT-only CT 변경은 ping 미호출)
  TC-11-03: 자신만 CT 에 ONLINE 으로 있을 때 ping 이 발생하지 않는다
            (self-exclusion — 자신을 새 노드로 오판해 ping loop 진입 방지)
"""
import os
import re
import time
from collections import Counter
from pathlib import Path

import pytest
from conftest import (
    CORE_BINARY, EDGE_BINARY, MQTT_HOST, MQTT_PORT,
    _clear_core_id_file, run_proc, wait_log,
)


# ─── 헬퍼 ────────────────────────────────────────────────────────────────────

def _ping_count(log_path: Path) -> int:
    """로그에서 'ping sent to' 출현 횟수"""
    if not log_path.exists():
        return 0
    return len(re.findall(r"\[edge\] ping sent to", log_path.read_text(errors="replace")))


def _ct_versions(log_path: Path) -> list[int]:
    """'CT applied (version=N)' 에서 버전 번호 목록 추출 (중복 포함)"""
    if not log_path.exists():
        return []
    return [
        int(m)
        for m in re.findall(
            r"\[edge\] CT applied \(version=(\d+)", log_path.read_text(errors="replace")
        )
    ]


# ─── TC-11-01: 동일 버전 CT 중복 적용 방지 ───────────────────────────────────

def test_ct_version_applied_at_most_once(backup_core, tmp_path):
    """
    Active 와 Backup Core 가 같은 CT 버전을 각각 publish 해도
    Edge 는 해당 버전을 정확히 한 번만 적용해야 한다.

    시나리오:
      1. Active + Backup Core 기동 (같은 broker → 두 프로세스 모두 topology publish)
      2. Edge 기동 → CT 수신
      3. 로그에서 'CT applied (version=N)' 의 N 별 출현 횟수가 모두 1 임을 검증

    ct_mutex 로 version 재확인 + replace 를 원자화하기 전에는 두 스레드가
    같은 버전의 CT 를 중복 적용해 'CT applied (version=N)' 이 두 줄 출력됐다.
    """
    _clear_core_id_file()
    log = tmp_path / "edge.log"
    os.environ["EDGE_ID_SUFFIX"] = "-race"
    os.environ["EDGE_NODE_PORT"] = str(MQTT_PORT)
    with run_proc(
        EDGE_BINARY, MQTT_HOST, str(MQTT_PORT), MQTT_HOST, str(MQTT_PORT),
        startup_log_pattern=r"\[edge\] CT applied",
        startup_timeout=15.0,
        log_path=log,
    ):
        os.environ.pop("EDGE_ID_SUFFIX", None)
        os.environ.pop("EDGE_NODE_PORT", None)

        # CT 가 여러 차례 갱신될 시간 확보
        assert wait_log(log, r"\[edge\] CT applied", timeout=15.0), \
            "Edge 가 CT 를 수신하지 못했습니다"
        time.sleep(3.0)

        versions = _ct_versions(log)
        assert versions, "CT applied 로그가 없습니다"

        duplicates = {v: c for v, c in Counter(versions).items() if c > 1}
        assert not duplicates, (
            f"동일 CT 버전이 두 번 이상 적용됨: {duplicates}\n"
            f"전체 버전 시퀀스: {versions}"
        )


# ─── TC-11-02: RTT 보고 후 추가 ping 미발생 ──────────────────────────────────

def test_no_ping_after_rtt_settled(active_core, tmp_path):
    """
    두 Edge 의 초기 ping/pong 교환(RTT 측정)이 완료된 뒤에는
    RTT 보고로 인해 CT 버전이 올라가더라도 추가 ping 이 발생해선 안 된다.

    시나리오:
      1. Active Core + Edge1 + Edge2 기동
      2. 상호 ping/pong → RTT 측정 → Core 에 보고 → CT version bump
      3. RTT 보고 직후 ping 횟수 snapshot
      4. 4초 대기 (이전 버그: 이 구간에서 ping 이 반복 발생)
      5. ping 횟수가 snapshot 과 동일함을 검증

    structural_change guard 이전에는 RTT-only CT 변경도 send_pings_to_nodes 를
    호출해 ping loop 가 발생했다.
    """
    log1 = tmp_path / "edge1.log"
    log2 = tmp_path / "edge2.log"

    os.environ["EDGE_ID_SUFFIX"] = "-r1"
    os.environ["EDGE_NODE_PORT"] = str(MQTT_PORT)
    with run_proc(
        EDGE_BINARY, MQTT_HOST, str(MQTT_PORT), MQTT_HOST, str(MQTT_PORT),
        startup_log_pattern=r"\[edge\] registered to",
        startup_timeout=10.0,
        log_path=log1,
    ):
        os.environ["EDGE_ID_SUFFIX"] = "-r2"
        os.environ["EDGE_NODE_PORT"] = str(MQTT_PORT + 1)
        with run_proc(
            EDGE_BINARY, MQTT_HOST, str(MQTT_PORT), MQTT_HOST, str(MQTT_PORT),
            startup_log_pattern=r"\[edge\] registered to",
            startup_timeout=10.0,
            log_path=log2,
        ):
            os.environ.pop("EDGE_ID_SUFFIX", None)
            os.environ.pop("EDGE_NODE_PORT", None)

            # 초기 ping/pong + RTT 보고 완료 대기
            assert wait_log(log1, r"pong from .+: RTT=", timeout=15.0), \
                "Edge1 RTT 측정 실패"
            assert wait_log(log2, r"pong from .+: RTT=", timeout=15.0), \
                "Edge2 RTT 측정 실패"

            # RTT 보고 → Core CT 갱신 → Edge 수신 정착 대기
            time.sleep(1.5)

            snap1 = _ping_count(log1)
            snap2 = _ping_count(log2)

            # 관찰 구간: 이전 버그에서는 이 사이에 ping 이 반복 발생
            time.sleep(4.0)

            after1 = _ping_count(log1)
            after2 = _ping_count(log2)

            assert after1 == snap1, (
                f"Edge1: RTT 정착 후 추가 ping 발생 ({snap1} → {after1})"
            )
            assert after2 == snap2, (
                f"Edge2: RTT 정착 후 추가 ping 발생 ({snap2} → {after2})"
            )


# ─── TC-11-03: 단독 Edge 에서 자신만 CT 에 등장할 때 ping 미발생 ─────────────

def test_no_ping_when_only_self_in_ct(active_core, tmp_path):
    """
    Edge 가 혼자 Core 에 등록되어 CT 의 유일한 ONLINE NODE 가 자신인 경우
    ping 이 발송되지 않아야 한다.

    시나리오:
      1. Active Core + Edge 1개 기동
      2. CT 수신 (ONLINE NODE = 자신 1개, CORE 노드는 ping 대상 제외)
      3. 2초 대기 후 'ping sent to' 가 로그에 없음을 검증

    self-exclusion 이전에는 structural_change 루프가 자신을 '새 ONLINE 노드'로
    인식해 send_pings_to_nodes 를 호출했다. (함수 내부에서 self skip 으로
    실제 ping 은 발송되지 않지만, RTT 보고 후 CT 갱신 시나리오에서 loop 의
    트리거가 됐다.)
    """
    os.environ["EDGE_ID_SUFFIX"] = "-solo"
    os.environ["EDGE_NODE_PORT"] = str(MQTT_PORT)
    log = tmp_path / "edge_solo.log"
    with run_proc(
        EDGE_BINARY, MQTT_HOST, str(MQTT_PORT), MQTT_HOST, str(MQTT_PORT),
        startup_log_pattern=r"\[edge\] CT applied",
        startup_timeout=10.0,
        log_path=log,
    ):
        os.environ.pop("EDGE_ID_SUFFIX", None)
        os.environ.pop("EDGE_NODE_PORT", None)

        assert wait_log(log, r"\[edge\] CT applied", timeout=10.0), \
            "Edge 가 CT 를 수신하지 못했습니다"
        time.sleep(2.0)

        count = _ping_count(log)
        assert count == 0, (
            f"단독 Edge CT 수신 후 불필요한 ping {count}회 발생"
        )
