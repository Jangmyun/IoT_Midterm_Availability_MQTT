"""
통합 테스트 공통 픽스처 및 헬퍼
- MqttSpy  : 토픽·페이로드를 수집하는 paho MQTT 클라이언트
- run_proc : 바이너리 프로세스 컨텍스트 매니저
- core_proc / edge_proc : pytest 픽스처
"""
import json
import os
import re
import signal
import subprocess
import time
import uuid
from contextlib import contextmanager
from pathlib import Path
from typing import Generator

import paho.mqtt.client as mqtt
import pytest

# ──────────────────────────────────────────────
# 환경 설정
# ──────────────────────────────────────────────
MQTT_HOST = os.environ.get("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))

PROJECT_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = Path(os.environ.get("BUILD_DIR", PROJECT_ROOT / "broker" / "build"))
CORE_BINARY = os.environ.get("CORE_BINARY", str(BUILD_DIR / "core_broker"))
EDGE_BINARY = os.environ.get("EDGE_BINARY", str(BUILD_DIR / "edge_broker"))


# ──────────────────────────────────────────────
# MqttSpy : 수신 메시지 누적 클라이언트
# ──────────────────────────────────────────────
class MqttSpy:
    """
    지정한 토픽을 구독해 수신된 모든 메시지를 보관.
    wait_for() 로 특정 패턴을 기다리고, payloads() 로 파싱된 JSON 목록을 반환한다.
    """

    def __init__(self, host: str = MQTT_HOST, port: int = MQTT_PORT):
        self._client = mqtt.Client(
            client_id=f"spy-{uuid.uuid4().hex[:8]}",
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        )
        self._client.on_message = self._on_message
        self._client.connect(host, port, keepalive=60)
        self._client.loop_start()
        # { topic: [payload_str, ...] }
        self._received: dict[str, list[str]] = {}

    def subscribe(self, *topics: str) -> "MqttSpy":
        for t in topics:
            self._client.subscribe(t, qos=1)
        return self

    def _on_message(self, client, userdata, msg):
        payload = msg.payload.decode(errors="replace")
        self._received.setdefault(msg.topic, []).append(payload)

    # ── 조회 ──────────────────────────────────
    def all_topics(self) -> list[str]:
        return list(self._received.keys())

    def raw(self, topic: str) -> list[str]:
        """주어진 토픽의 raw 페이로드 목록"""
        return self._received.get(topic, [])

    def payloads(self, topic: str) -> list[dict]:
        """JSON 파싱된 페이로드 목록"""
        return [json.loads(p) for p in self.raw(topic)]

    def matching_topics(self, pattern: str) -> list[str]:
        """MQTT 와일드카드(# +)를 정규식으로 변환해 일치하는 토픽 목록 반환"""
        regex = re.escape(pattern).replace(r"\#", ".*").replace(r"\+", "[^/]+")
        regex = f"^{regex}$"
        return [t for t in self._received if re.match(regex, t)]

    def count(self, topic_pattern: str) -> int:
        """패턴에 일치하는 모든 메시지 수"""
        return sum(
            len(self._received[t]) for t in self.matching_topics(topic_pattern)
        )

    # ── 대기 ──────────────────────────────────
    def wait_for(
        self,
        topic_pattern: str,
        timeout: float = 10.0,
        min_count: int = 1,
    ) -> bool:
        """
        topic_pattern에 일치하는 메시지가 min_count개 이상 수신될 때까지 대기.
        timeout(초) 안에 조건이 충족되면 True, 아니면 False.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.count(topic_pattern) >= min_count:
                return True
            time.sleep(0.05)
        return False

    def wait_payload(
        self,
        topic: str,
        check,
        timeout: float = 10.0,
    ) -> dict | None:
        """
        topic에서 check(payload_dict) → True 인 페이로드가 수신될 때까지 대기.
        찾으면 해당 페이로드 반환, timeout 시 None.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for p in self.payloads(topic):
                try:
                    if check(p):
                        return p
                except Exception:
                    pass
            time.sleep(0.05)
        return None

    def clear(self):
        self._received.clear()

    def close(self):
        self._client.loop_stop()
        self._client.disconnect()


# ──────────────────────────────────────────────
# 프로세스 관리
# ──────────────────────────────────────────────
@contextmanager
def run_proc(
    *args: str,
    startup_log_pattern: str | None = None,
    startup_timeout: float = 10.0,
    log_path: Path | None = None,
) -> Generator[subprocess.Popen, None, None]:
    """
    바이너리를 subprocess로 기동하고 컨텍스트가 끝나면 SIGTERM → 종료.
    startup_log_pattern 이 주어지면 stdout에서 해당 패턴이 나올 때까지 대기.
    """
    log_file = open(log_path, "w") if log_path else subprocess.PIPE
    proc = subprocess.Popen(
        list(args),
        stdout=log_file,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    if startup_log_pattern and log_path:
        # log_path에 출력 — 파일을 읽으며 패턴 대기
        deadline = time.monotonic() + startup_timeout
        found = False
        while time.monotonic() < deadline:
            if log_path.exists():
                content = log_path.read_text(errors="replace")
                if re.search(startup_log_pattern, content):
                    found = True
                    break
            time.sleep(0.1)
        if not found:
            proc.terminate()
            proc.wait(timeout=3)
            content = log_path.read_text(errors="replace") if log_path.exists() else ""
            raise RuntimeError(
                f"Process {args[0]} did not produce '{startup_log_pattern}' "
                f"within {startup_timeout}s.\nLog:\n{content[-800:]}"
            )

    try:
        yield proc
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        if log_path and not isinstance(log_file, int):
            log_file.close()


# ──────────────────────────────────────────────
# retained 메시지 클리어 헬퍼
# ──────────────────────────────────────────────
_RETAINED_TOPICS = [
    "campus/monitor/topology",
    "_core/sync/connection_table",
]


def _clear_retained(host: str = MQTT_HOST, port: int = MQTT_PORT) -> None:
    """retained 메시지를 빈 페이로드로 덮어써서 삭제한다."""
    c = mqtt.Client(
        client_id=f"cleanup-{uuid.uuid4().hex[:8]}",
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
    )
    c.connect(host, port, keepalive=10)
    c.loop_start()
    for topic in _RETAINED_TOPICS:
        c.publish(topic, payload=None, qos=1, retain=True)
    time.sleep(0.3)
    c.loop_stop()
    c.disconnect()


@pytest.fixture(scope="session", autouse=True)
def clear_retained_before_session():
    """테스트 세션 시작 전에 오래된 retained 메시지를 클리어한다."""
    _clear_retained()
    yield
    _clear_retained()


# ──────────────────────────────────────────────
# pytest 픽스처
# ──────────────────────────────────────────────
@pytest.fixture
def spy(tmp_path):
    """MqttSpy 인스턴스 — 테스트마다 새로 생성"""
    s = MqttSpy()
    yield s
    s.close()


@pytest.fixture
def log_dir(tmp_path):
    return tmp_path


@pytest.fixture
def active_core(tmp_path):
    """Active Core 기동 픽스처. 시작 전 retained CT 클리어로 stale 버전 충돌 방지."""
    _clear_retained()
    log = tmp_path / "core_active.log"
    with run_proc(
        CORE_BINARY, MQTT_HOST, str(MQTT_PORT),
        startup_log_pattern=r"\[core\] connected \(ACTIVE\)",
        startup_timeout=10.0,
        log_path=log,
    ) as proc:
        yield proc, log
    _clear_retained()


@pytest.fixture
def backup_core(tmp_path, active_core):
    """Backup Core 기동 픽스처 (Active 기동 후 사용)."""
    log = tmp_path / "core_backup.log"
    with run_proc(
        CORE_BINARY, MQTT_HOST, str(MQTT_PORT), MQTT_HOST, str(MQTT_PORT),
        startup_log_pattern=r"\[core\] connected \(BACKUP\)",
        startup_timeout=10.0,
        log_path=log,
    ) as proc:
        yield proc, log


@pytest.fixture
def edge(tmp_path, active_core):
    """Edge 기동 픽스처 (Active Core 필요)."""
    log = tmp_path / "edge.log"
    with run_proc(
        EDGE_BINARY, MQTT_HOST, str(MQTT_PORT), MQTT_HOST, str(MQTT_PORT),
        startup_log_pattern=r"\[edge\] registered to",
        startup_timeout=10.0,
        log_path=log,
    ) as proc:
        yield proc, log


# ──────────────────────────────────────────────
# 메시지 생성 헬퍼
# ──────────────────────────────────────────────
def make_event(
    event_type: str = "INTRUSION",
    priority: str = "HIGH",
    building_id: str = "bldg-a",
    camera_id: str = "cam-01",
    msg_id: str | None = None,
) -> dict:
    msg_id = msg_id or str(uuid.uuid4())
    return {
        "msg_id": msg_id,
        "type": event_type,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "priority": priority,
        "source": {"role": "NODE", "id": str(uuid.uuid4())},
        "target": {"role": "CORE", "id": "all"},
        "route": {
            "original_node": str(uuid.uuid4()),
            "prev_hop": str(uuid.uuid4()),
            "next_hop": "all",
            "hop_count": 1,
            "ttl": 5,
        },
        "delivery": {"qos": 1, "dup": False, "retain": False},
        "payload": {
            "building_id": building_id,
            "camera_id": camera_id,
            "description": "test event",
        },
    }


def publish(client: mqtt.Client, topic: str, payload: dict, qos: int = 1) -> None:
    client.publish(topic, json.dumps(payload), qos=qos)


def make_publisher(host: str = MQTT_HOST, port: int = MQTT_PORT) -> mqtt.Client:
    """이벤트 발행용 단순 paho 클라이언트"""
    c = mqtt.Client(
        client_id=f"pub-{uuid.uuid4().hex[:8]}",
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
    )
    c.connect(host, port, keepalive=60)
    c.loop_start()
    return c


def make_status_msg(
    node_id: str,
    description: str = "",
) -> dict:
    """STATUS 타입 메시지 (노드 등록: description="ip:port", LWT 시뮬: description="")"""
    return {
        "msg_id": str(uuid.uuid4()),
        "type": "STATUS",
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source": {"role": "NODE", "id": node_id},
        "target": {"role": "CORE", "id": ""},
        "route": {
            "original_node": node_id,
            "prev_hop": node_id,
            "next_hop": "",
            "hop_count": 0,
            "ttl": 1,
        },
        "delivery": {"qos": 1, "dup": False, "retain": False},
        "payload": {"building_id": "", "camera_id": "", "description": description},
    }


def wait_log(log_path: Path, pattern: str, timeout: float = 10.0) -> bool:
    """로그 파일에서 regex 패턴이 나타날 때까지 대기. 발견 시 True."""
    import re as _re
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log_path.exists():
            if _re.search(pattern, log_path.read_text(errors="replace")):
                return True
        time.sleep(0.1)
    return False
