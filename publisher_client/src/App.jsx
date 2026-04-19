import { useState, useCallback, useRef } from 'react';
import { usePublisher } from './hooks/usePublisher.js';
import { CameraView }   from './components/CameraView.jsx';
import { EventLog }     from './components/EventLog.jsx';
import { MSG_TYPES }    from './mqtt/message.js';
import { generateUuid } from './utils/uuid.js';
import './App.css';

const DEFAULT_URL      = 'ws://localhost:9001';
const DEFAULT_BUILDING = 'building-a';
const DEFAULT_CAMERA   = 'cam-01';
const PUBLISHER_ID     = generateUuid();
const MOTION_COOLDOWN  = 3000; // 모션 이벤트 최소 간격 (ms)

const STATUS_LABEL = {
  connecting:   '연결 중...',
  connected:    '연결됨',
  disconnected: '연결 끊김',
  error:        '오류',
};

export default function App() {
  const {
    status,
    events,
    activeBrokerUrl,
    queueSize,
    ctReceived,
    onFallback,
    connect,
    disconnect,
    publish,
  } = usePublisher();

  const [urlInput,    setUrlInput]    = useState(DEFAULT_URL);
  const [buildingId,  setBuildingId]  = useState(DEFAULT_BUILDING);
  const [cameraId,    setCameraId]    = useState(DEFAULT_CAMERA);
  const [motionOn,    setMotionOn]    = useState(false);   // 자동 감지 on/off
  const [sensitivity, setSensitivity] = useState(2);       // 감지 민감도 (%)
  const lastMotionRef = useRef(0);

  // 연결 / 해제 토글
  function handleConnect() {
    if (status === 'connected') {
      disconnect();
    } else {
      connect(urlInput);
    }
  }

  // 수동 이벤트 발행
  function handleManual(type) {
    publish({ publisherId: PUBLISHER_ID, type, buildingId, cameraId, description: 'manual' });
  }

  // 자동 모션 감지 콜백 (쿨다운 적용)
  const handleMotion = useCallback(() => {
    const now = Date.now();
    if (now - lastMotionRef.current < MOTION_COOLDOWN) return;
    lastMotionRef.current = now;
    publish({
      publisherId: PUBLISHER_ID,
      type:        MSG_TYPES.MOTION,
      buildingId,
      cameraId,
      description: 'auto-detect',
    });
  }, [publish, buildingId, cameraId]);

  const isConnected = status === 'connected';

  return (
    <div className="app">
      {/* ── 헤더 ─────────────────────────────────────────────────── */}
      <header className="app-header">
        <h1 className="app-title">IoT Publisher</h1>
        <span className={`badge status-badge status-badge--${status}`}>
          {STATUS_LABEL[status]}
        </span>
      </header>

      <main className="app-body">
        {/* ── 연결 설정 ─────────────────────────────────────────── */}
        <section className="card">
          <h2 className="card__title">연결 설정</h2>
          <div className="field-row">
            <label className="field-label">Broker WebSocket URL (Edge 주소)</label>
            <input
              className="field-input"
              value={urlInput}
              onChange={e => setUrlInput(e.target.value)}
              placeholder="ws://192.168.0.9:9001"
              disabled={isConnected}
            />
          </div>
          <div className="field-row field-row--two">
            <div>
              <label className="field-label">빌딩 ID</label>
              <input
                className="field-input"
                value={buildingId}
                onChange={e => setBuildingId(e.target.value)}
              />
            </div>
            <div>
              <label className="field-label">카메라 ID</label>
              <input
                className="field-input"
                value={cameraId}
                onChange={e => setCameraId(e.target.value)}
              />
            </div>
          </div>
          <div className="field-row field-row--hint">
            <span className="hint">Publisher ID: {PUBLISHER_ID.slice(0, 8)}…</span>
          </div>
          <div className="field-row field-row--hint">
            <span className="hint">
              Active broker: {activeBrokerUrl || '—'}
              {onFallback && <span className="failover-badge"> · Failover 중</span>}
            </span>
          </div>
          <div className="field-row field-row--hint">
            <span className="hint">
              CT: {isConnected
                ? (ctReceived
                    ? <span className="ct-ok">수신됨 ✓</span>
                    : <span className="ct-waiting">대기 중…</span>)
                : '—'}
            </span>
          </div>
          <div className="field-row field-row--hint">
            <span className="hint">
              Queued events: {queueSize}
            </span>
          </div>
          <button
            className={`btn btn--block ${isConnected ? 'btn--ghost' : 'btn--primary'}`}
            onClick={handleConnect}
          >
            {isConnected ? '연결 해제' : '연결'}
          </button>
        </section>

        {/* ── 카메라 + 모션 감지 ───────────────────────────────── */}
        <section className="card">
          <div className="card__title-row">
            <h2 className="card__title">카메라</h2>
            <label className="toggle">
              <input
                type="checkbox"
                checked={motionOn}
                onChange={e => setMotionOn(e.target.checked)}
                disabled={!isConnected}
              />
              <span className="toggle__label">자동 감지</span>
            </label>
          </div>

          <CameraView
            onMotion={handleMotion}
            enabled={motionOn && isConnected}
            sensitivity={sensitivity}
          />

          <div className="field-row">
            <label className="field-label">
              감지 민감도: <strong>{sensitivity}%</strong>
            </label>
            <input
              type="range"
              min={1}
              max={20}
              value={sensitivity}
              onChange={e => setSensitivity(Number(e.target.value))}
              className="slider"
            />
          </div>
        </section>

        {/* ── 수동 발행 ─────────────────────────────────────────── */}
        <section className="card">
          <h2 className="card__title">수동 발행</h2>
          <div className="manual-btns">
            <button
              className="btn btn--motion"
              onClick={() => handleManual(MSG_TYPES.MOTION)}
              disabled={!isConnected}
            >
              모션
            </button>
            <button
              className="btn btn--door"
              onClick={() => handleManual(MSG_TYPES.DOOR_FORCED)}
              disabled={!isConnected}
            >
              강제 개문
            </button>
            <button
              className="btn btn--intrusion"
              onClick={() => handleManual(MSG_TYPES.INTRUSION)}
              disabled={!isConnected}
            >
              침입
            </button>
          </div>
        </section>

        {/* ── 이벤트 로그 ───────────────────────────────────────── */}
        <section className="card">
          <div className="card__title-row">
            <h2 className="card__title">발행 로그</h2>
            <span className="hint">{events.length}건</span>
          </div>
          <EventLog events={events} />
        </section>
      </main>
    </div>
  );
}
