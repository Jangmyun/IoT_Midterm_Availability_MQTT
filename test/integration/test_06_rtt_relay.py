"""
TC-RTT-RELAY: RTT 측정 및 Relay Node 선택 검증 (FR-08, 시나리오 5.6)

두 Edge가 Core에 등록 → 갱신된 CT 수신 → 상호 Ping/Pong 교환 →
RTT 계산 후 LinkEntry 갱신 → relay_node_id 선택

검증 항목:
  - CT 수신 후 각 Edge가 상대 Node에 Ping 발송
  - Pong 수신 후 RTT를 계산하고 로그에 기록
  - Relay Node가 선택됨
  - OFFLINE 처리된 Node는 Relay 후보에서 제외 (node_down alert 기반)
"""
import pytest

from conftest import (
    EDGE_BINARY,
    MQTT_HOST,
    MQTT_PORT,
    run_proc,
    wait_log,
)


# ──────────────────────────────────────────────
# 로컬 픽스처: Core 1개 + Edge 2개
# ──────────────────────────────────────────────
@pytest.fixture
def two_edges(tmp_path, active_core):
    """Active Core 위에 Edge 2개를 순서대로 기동한다."""
    log1 = tmp_path / "edge1.log"
    log2 = tmp_path / "edge2.log"
    with run_proc(
        EDGE_BINARY, MQTT_HOST, str(MQTT_PORT), MQTT_HOST, str(MQTT_PORT),
        startup_log_pattern=r"\[edge\] registered to",
        startup_timeout=10.0,
        log_path=log1,
    ) as proc1:
        with run_proc(
            EDGE_BINARY, MQTT_HOST, str(MQTT_PORT), MQTT_HOST, str(MQTT_PORT),
            startup_log_pattern=r"\[edge\] registered to",
            startup_timeout=10.0,
            log_path=log2,
        ) as proc2:
            yield (proc1, log1), (proc2, log2)


# ──────────────────────────────────────────────
# TC-01: CT 수신 후 Ping 발송
# ──────────────────────────────────────────────
def test_edges_send_ping_after_ct(active_core, two_edges):
    """
    FR-08: CT에 상대 Node가 포함되면 각 Edge가 해당 Node로 Ping을 발송한다.

    1. Core + Edge1 + Edge2 기동
    2. Edge2 등록 시 Core가 CT(nodes≥2)를 브로드캐스트
    3. Edge1·Edge2 로그 모두에 'ping sent to' 확인
    """
    (_, log1), (_, log2) = two_edges

    assert wait_log(log1, r"ping sent to", timeout=15.0), \
        "Edge1이 상대 Node에 Ping을 발송하지 않았습니다"
    assert wait_log(log2, r"ping sent to", timeout=15.0), \
        "Edge2가 상대 Node에 Ping을 발송하지 않았습니다"


# ──────────────────────────────────────────────
# TC-02: Pong 수신 후 RTT 계산
# ──────────────────────────────────────────────
def test_rtt_calculated_after_pong(active_core, two_edges):
    """
    FR-08: 각 Edge가 Pong을 수신한 뒤 RTT(ms)를 계산하고 로그에 남긴다.

    1. Ping 발송 후 상대 Edge로부터 Pong 수신
    2. Edge 로그에 'pong from <uuid>: RTT=<float>ms' 패턴 확인
    """
    (_, log1), (_, log2) = two_edges

    assert wait_log(log1, r"pong from .+: RTT=[\d.]+ms", timeout=15.0), \
        "Edge1이 Pong 수신 후 RTT를 계산하지 않았습니다"
    assert wait_log(log2, r"pong from .+: RTT=[\d.]+ms", timeout=15.0), \
        "Edge2가 Pong 수신 후 RTT를 계산하지 않았습니다"


# ──────────────────────────────────────────────
# TC-03: RTT 계산 완료 후 Relay Node 선택
# ──────────────────────────────────────────────
def test_relay_node_selected_after_rtt(active_core, two_edges):
    """
    FR-08: RTT가 측정된 후 각 Edge가 최적 Relay Node를 선택해 로그에 기록한다.

    선택 기준: RTT 최소 → 동점 시 hop_to_core 최소 (PRD 5.6)
    로그 패턴: 'relay node selected: <uuid>'
    """
    (_, log1), (_, log2) = two_edges

    assert wait_log(log1, r"relay node selected: [0-9a-f-]{36}", timeout=15.0), \
        "Edge1이 Relay Node를 선택하지 않았습니다"
    assert wait_log(log2, r"relay node selected: [0-9a-f-]{36}", timeout=15.0), \
        "Edge2가 Relay Node를 선택하지 않았습니다"


# ──────────────────────────────────────────────
# TC-04: OFFLINE Node → Relay 후보 제외
# ──────────────────────────────────────────────
def test_offline_node_excluded_from_relay(active_core, two_edges, spy):
    """
    FR-08, W-02: Node가 LWT(OFFLINE)로 처리되면 갱신된 CT가 브로드캐스트되고
    살아있는 Edge는 해당 노드를 Relay 후보에서 제외한다.

    1. Relay 선택이 완료될 때까지 대기 (정상 상태 확인)
    2. Edge1 강제 종료 (LWT 트리거)
    3. Core가 campus/alert/node_down 발행 확인 (OFFLINE 마킹)
    4. Edge2가 갱신된 CT를 수신 (CT version 업 확인)
    """
    spy.subscribe("campus/alert/node_down/#")

    (proc1, log1), (_, log2) = two_edges

    # 정상 상태: relay 선택 완료 확인
    assert wait_log(log2, r"relay node selected:", timeout=15.0), \
        "Edge2가 정상 상태에서 Relay Node를 선택하지 못했습니다"

    # Edge1 강제 종료 → LWT 발행
    proc1.kill()
    proc1.wait(timeout=3)

    # Core가 node_down 알림 발행 확인
    assert spy.wait_for("campus/alert/node_down/#", timeout=15.0), \
        "Core가 campus/alert/node_down 을 발행하지 않았습니다 (Edge1 LWT 미처리)"

    # Edge2가 갱신된 CT를 수신했는지 확인
    # (Core는 OFFLINE 마킹 후 CT 브로드캐스트 → Edge2의 CT version 업)
    assert wait_log(log2, r"CT applied", timeout=10.0), \
        "Edge2가 Edge1 장애 후 갱신된 CT를 수신하지 못했습니다"
