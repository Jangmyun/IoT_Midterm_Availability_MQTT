"""
TC-10 CORE-REJOIN: Core ID 영속화 및 Backup 재진입 검증

검증 항목:
  - Core 최초 실행 시 /tmp/core_id_<port>.txt 생성
  - Core 재시작 후 동일 core_id 유지 (파일에서 복원)
  - 재시작된 Core 가 Backup 모드로도 동일 UUID 사용
"""
import re
import time
from pathlib import Path

import pytest
from conftest import (
    MQTT_HOST, MQTT_PORT, CORE_BINARY,
    run_proc, wait_log, _clear_retained,
)

CORE_ID_FILE = Path(f"/tmp/core_id_{MQTT_PORT}.txt")
UUID_RE = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")


def _extract_running_uuid(log_path: Path) -> str | None:
    """[core] <uuid> (ACTIVE|BACKUP) running 패턴에서 UUID 추출"""
    m = UUID_RE.search(
        re.sub(
            r".*\[core\] ([0-9a-f-]{36}) \((ACTIVE|BACKUP)\) running.*",
            r"\1",
            log_path.read_text(errors="replace"),
            flags=re.DOTALL,
        )
    )
    # 위 방식 대신 직접 검색
    for line in log_path.read_text(errors="replace").splitlines():
        m2 = re.match(r"\[core\] ([0-9a-f-]{36}) \((ACTIVE|BACKUP)\) running", line)
        if m2:
            return m2.group(1)
    return None


@pytest.fixture(autouse=True)
def isolate_core_id_file():
    """각 테스트 전후 core_id 파일과 retained 메시지 정리"""
    CORE_ID_FILE.unlink(missing_ok=True)
    _clear_retained()
    yield
    CORE_ID_FILE.unlink(missing_ok=True)
    _clear_retained()


def test_core_id_file_created_on_first_start(tmp_path):
    """
    A-1: Core 최초 실행 시 /tmp/core_id_<port>.txt 파일이 생성된다.
    """
    assert not CORE_ID_FILE.exists(), "테스트 전 파일이 없어야 합니다"

    log = tmp_path / "core.log"
    with run_proc(
        CORE_BINARY, MQTT_HOST, str(MQTT_PORT),
        startup_log_pattern=r"\[core\] generated new core_id",
        startup_timeout=10.0,
        log_path=log,
    ):
        assert wait_log(log, r"\[core\] generated new core_id", timeout=5.0), \
            "generated new core_id 로그가 출력되지 않았습니다"
        assert CORE_ID_FILE.exists(), \
            f"{CORE_ID_FILE} 파일이 생성되지 않았습니다"
        saved = CORE_ID_FILE.read_text().strip()
        assert UUID_RE.fullmatch(saved), \
            f"저장된 UUID 형식이 올바르지 않습니다: {saved!r}"


def test_core_id_persisted_across_restarts(tmp_path):
    """
    A-1: Core 재시작 후에도 동일한 core_id 를 사용한다.
    """
    log1 = tmp_path / "core1.log"
    log2 = tmp_path / "core2.log"

    # 1차 기동 — 새 UUID 생성
    with run_proc(
        CORE_BINARY, MQTT_HOST, str(MQTT_PORT),
        startup_log_pattern=r"\[core\] generated new core_id",
        startup_timeout=10.0,
        log_path=log1,
    ):
        assert wait_log(log1, r"\[core\] connected \(ACTIVE\)", timeout=10.0), \
            "1차 기동에서 ACTIVE 연결 로그가 없습니다"

    uuid1 = CORE_ID_FILE.read_text().strip()
    assert UUID_RE.fullmatch(uuid1), f"1차 UUID 형식 오류: {uuid1!r}"

    time.sleep(0.5)  # TIME_WAIT 방지

    # 2차 기동 — 파일에서 UUID 복원
    with run_proc(
        CORE_BINARY, MQTT_HOST, str(MQTT_PORT),
        startup_log_pattern=r"\[core\] restored core_id from file",
        startup_timeout=10.0,
        log_path=log2,
    ):
        assert wait_log(log2, r"\[core\] connected \(ACTIVE\)", timeout=10.0), \
            "2차 기동에서 ACTIVE 연결 로그가 없습니다"

    uuid2 = CORE_ID_FILE.read_text().strip()

    assert uuid1 == uuid2, \
        f"재시작 후 core_id 변경됨: {uuid1} → {uuid2}"
    assert wait_log(log2, r"restored core_id from file", timeout=1.0), \
        "파일 복원 로그가 출력되지 않았습니다"


def test_core_id_same_in_backup_mode(tmp_path):
    """
    A-1: Backup 모드로 재시작해도 동일 core_id 를 사용한다.
    이전 Active 가 Backup 으로 재진입 시 UUID 연속성 보장.
    """
    log1 = tmp_path / "core_active.log"
    log2 = tmp_path / "core_backup.log"

    # 1차: Active 로 기동 → UUID 생성
    with run_proc(
        CORE_BINARY, MQTT_HOST, str(MQTT_PORT),
        startup_log_pattern=r"\[core\] generated new core_id",
        startup_timeout=10.0,
        log_path=log1,
    ) as proc:
        assert wait_log(log1, r"\[core\] connected \(ACTIVE\)", timeout=10.0), \
            "1차 Active 기동 실패"
        uuid1 = CORE_ID_FILE.read_text().strip()
        # SIGKILL (LWT 트리거 없음 — 단순 재진입 시나리오)
        proc.kill()
        proc.wait(timeout=3)

    time.sleep(0.5)

    # 2차: Backup 모드로 재시작 (동일 브로커를 Active 로 가리킴)
    with run_proc(
        CORE_BINARY, MQTT_HOST, str(MQTT_PORT),
        MQTT_HOST, str(MQTT_PORT),          # argc=5: Backup 모드
        startup_log_pattern=r"\[core\] restored core_id from file",
        startup_timeout=10.0,
        log_path=log2,
    ):
        assert wait_log(log2, r"restored core_id from file", timeout=5.0), \
            "Backup 재시작에서 파일 복원 로그가 없습니다"

    uuid2 = CORE_ID_FILE.read_text().strip()
    assert uuid1 == uuid2, \
        f"Backup 재진입 시 UUID 변경됨: {uuid1} → {uuid2}"
