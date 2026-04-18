import { useEffect, useState } from 'react';
import { useMqtt } from './hooks/useMqtt.js';
import TopologyGraph from './components/TopologyGraph.jsx';
import './App.css';

// 이벤트 타입별 배지 색상
const EVENT_TYPE_COLOR = {
  INTRUSION:    'badge--red',
  DOOR_FORCED:  'badge--orange',
  MOTION:       'badge--yellow',
  LWT_CORE:     'badge--purple',
  LWT_NODE:     'badge--purple',
  STATUS:       'badge--gray',
  RELAY:        'badge--gray',
};

const INCIDENT_BADGE = {
  ACTIVE_CORE_DOWN: 'badge--red',
  BACKUP_CORE_DOWN: 'badge--orange',
  CORE_DOWN: 'badge--red',
  FAILOVER_SWITCH: 'badge--purple',
  EDGE_DOWN: 'badge--orange',
  EDGE_UP: 'badge--green',
};

function reconnectReasonLabel(reason) {
  if (reason === 'W-01') return 'LWT';
  if (reason === 'M-04') return 'topology sync';
  return 'core_switch';
}

function formatIncidentTime(ts) {
  if (!ts) return '—';
  return new Date(ts).toLocaleTimeString('ko-KR', {
    hour12: false,
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
}

function formatIncidentText(incident) {
  const shortId = incident.nodeId ? `${incident.nodeId.slice(0, 8)}…` : '(unknown)';

  if (incident.type === 'CORE_DOWN') {
    return `Core ${shortId} disconnected`;
  }
  if (incident.type === 'ACTIVE_CORE_DOWN') {
    return `Active core ${shortId} disconnected`;
  }
  if (incident.type === 'BACKUP_CORE_DOWN') {
    return `Backup core ${shortId} disconnected`;
  }
  if (incident.type === 'FAILOVER_SWITCH') {
    return `Backup promoted to active${incident.endpoint ? ` (${incident.endpoint})` : ''}`;
  }
  if (incident.type === 'EDGE_DOWN') {
    return `Edge ${shortId} disconnected`;
  }
  if (incident.type === 'EDGE_UP') {
    return `Edge ${shortId} recovered`;
  }
  return shortId;
}

// nodes를 CORE 우선 → ONLINE → OFFLINE 순으로 정렬
function sortNodes(nodes) {
  return [...nodes].sort((a, b) => {
    if (a.role !== b.role) return a.role === 'CORE' ? -1 : 1;
    if (a.status !== b.status) return a.status === 'ONLINE' ? -1 : 1;
    return 0;
  });
}

export default function App() {
  const { status, topology, events, alerts, incidents, reconnectInfo, brokerUrl, setBrokerUrl } = useMqtt();

  // 브로커 주소 입력 state
  const [urlInput, setUrlInput] = useState(brokerUrl);

  useEffect(() => {
    setUrlInput(brokerUrl);
  }, [brokerUrl]);

  // 노드 선택 state
  const [selectedNodeId, setSelectedNodeId] = useState(null);
  const selectedNode = topology?.nodes.find(n => n.id === selectedNodeId) ?? null;

  // 필터 state
  const [filterType,     setFilterType]     = useState('ALL');
  const [filterBuilding, setFilterBuilding] = useState('ALL');
  const [filterHighOnly, setFilterHighOnly] = useState(false);

  // 필터 옵션 (수신된 events에서 동적 추출)
  const eventTypes = ['ALL', ...new Set(events.map(e => e.type).filter(Boolean))];
  const buildings  = ['ALL', ...new Set(events.map(e => e.payload?.building_id).filter(Boolean))];

  // 필터 적용
  const filteredEvents = events.filter(e => {
    if (filterType     !== 'ALL' && e.type                  !== filterType)     return false;
    if (filterBuilding !== 'ALL' && e.payload?.building_id  !== filterBuilding) return false;
    if (filterHighOnly && e.priority !== 'HIGH')                                return false;
    return true;
  });

  // Stats (computed)
  const onlineCount   = topology?.nodes.filter(n => n.status === 'ONLINE').length  ?? 0;
  const offlineCount  = topology?.nodes.filter(n => n.status === 'OFFLINE').length ?? 0;
  const criticalCount = events.filter(e => e.priority === 'HIGH').length;
  const latestCoreIncident = incidents.find(incident => incident.role === 'CORE') ?? null;

  return (
    <div className="monitor">

      {/* ── 헤더 ─────────────────────────────────────────────── */}
      <header className="monitor-header">
        <h1>Smart Campus Monitor</h1>
        <span className={`status status--${status}`}>{status}</span>
        <form
          className="broker-url-form"
          onSubmit={e => { e.preventDefault(); setBrokerUrl(urlInput.trim()); }}
        >
          <input
            className="broker-url-input"
            value={urlInput}
            onChange={e => setUrlInput(e.target.value)}
            placeholder="ws://localhost:9001"
            spellCheck={false}
          />
          <button className="broker-url-btn" type="submit">Connect</button>
        </form>
        {topology && (
          <span className="core-info">
            Active: <code title={topology.active_core_id}>{topology.active_core_id.slice(0, 8)}…</code>
            &nbsp;|&nbsp;
            Backup: <code title={topology.backup_core_id || 'none'}>{topology.backup_core_id ? topology.backup_core_id.slice(0, 8) + '…' : '(none)'}</code>
            &nbsp;|&nbsp;v{topology.version}
            &nbsp;|&nbsp;{topology.last_update}
          </span>
        )}
      </header>

      {/* ── Core 재연결 배너 ──────────────────────────────────────── */}
      {reconnectInfo && (
        <div className="reconnect-banner">
          <span className="reconnect-spinner" />
          <span>
            Core failover ({reconnectReasonLabel(reconnectInfo.reason)})
            &nbsp;— connecting to backup at <code>{reconnectInfo.url}</code>
          </span>
        </div>
      )}

      {/* ── Alert 배너 ────────────────────────────────────────── */}
      {alerts.length > 0 && (
        <div className="alert-banner">
          {alerts.map((a, i) => (
            <div key={i} className={`alert-item alert-item--${a.topic.includes('node_down') ? 'down' : a.topic.includes('node_up') ? 'up' : 'core'}`}>
              <strong>{a.topic}</strong>
              {/* node_down / node_up: CT 페이로드 → nodeId + CT version 표시 */}
              {a.nodeId && <span> — node: {a.nodeId.slice(0, 8)}…{a.ct ? ` (ct.v${a.ct.version})` : ''}</span>}
              {/* core_switch / will/core: MqttMessage 페이로드 */}
              {!a.nodeId && a.msg && <span> — {a.msg.type}{a.msg.payload?.description ? `: ${a.msg.payload.description}` : ''}</span>}
            </div>
          ))}
        </div>
      )}

      {/* ── Stats row ─────────────────────────────────────────── */}
      <div className="stats-row">
        <div className="stat-card">
          <span className="stat-value stat-value--green">{onlineCount}</span>
          <span className="stat-label">Online</span>
        </div>
        <div className="stat-card">
          <span className="stat-value stat-value--red">{offlineCount}</span>
          <span className="stat-label">Offline</span>
        </div>
        <div className="stat-card">
          <span className="stat-value">{events.length}</span>
          <span className="stat-label">Events</span>
        </div>
        <div className="stat-card">
          <span className="stat-value stat-value--orange">{criticalCount}</span>
          <span className="stat-label">Critical</span>
        </div>
      </div>

      {(latestCoreIncident || incidents.length > 0) && (
        <section className="incident-section">
          <div className="incident-header">
            <h2>System Notices</h2>
            {latestCoreIncident && (
              <span className="incident-summary">
                {formatIncidentText(latestCoreIncident)} at {formatIncidentTime(latestCoreIncident.ts)}
              </span>
            )}
          </div>
          <div className="incident-list">
            {incidents.slice(0, 5).map((incident) => (
              <div key={incident.key} className="incident-item">
                <span className={`badge ${INCIDENT_BADGE[incident.type] ?? 'badge--gray'}`}>
                  {incident.type.replace('_', ' ')}
                </span>
                <span className="incident-text">{formatIncidentText(incident)}</span>
                <span className="incident-time">{formatIncidentTime(incident.ts)}</span>
              </div>
            ))}
          </div>
        </section>
      )}

      {/* ── Cytoscape 토폴로지 그래프 ─────────────────────────── */}
      <section className="graph-section">
        <h2>Topology{selectedNode ? ` — ${selectedNode.id.slice(0, 8)}… 선택됨` : ''}</h2>
        <TopologyGraph topology={topology} onNodeClick={setSelectedNodeId} />

        {/* 노드 상세 패널 */}
        {selectedNode && (
          <div className="node-detail">
            <div className="node-detail-header">
              <span className={`badge ${selectedNode.role === 'CORE' ? 'badge--purple' : 'badge--gray'}`}>
                {selectedNode.role}
              </span>
              <span className={`badge ${selectedNode.status === 'ONLINE' ? 'badge--green' : 'badge--red'}`}>
                {selectedNode.status}
              </span>
              <span className="node-detail-title" title={selectedNode.id}>
                {selectedNode.id}
              </span>
              <button className="node-detail-close" onClick={() => setSelectedNodeId(null)}>✕</button>
            </div>
            <div className="node-detail-body">
              <span className="node-detail-kv"><span>IP:Port</span><code>{selectedNode.ip}:{selectedNode.port}</code></span>
              <span className="node-detail-kv"><span>Hop to Core</span><code>{selectedNode.hop_to_core}</code></span>
            </div>
          </div>
        )}
      </section>

      {/* ── 하단 2열: Broker 카드 | 이벤트 로그 ──────────────── */}
      <div className="bottom-row">

        {/* Broker 상태 카드 */}
        <section className="broker-cards">
          <h2>Brokers ({topology?.nodes.length ?? 0})</h2>
          <div className="card-list">
            {topology
              ? sortNodes(topology.nodes).map(n => (
                <div key={n.id} className={`broker-card broker-card--${n.status.toLowerCase()}`}>
                  <div className="broker-card-header">
                    <span className={`badge ${n.role === 'CORE' ? 'badge--purple' : 'badge--gray'}`}>{n.role}</span>
                    <span className={`badge ${n.status === 'ONLINE' ? 'badge--green' : 'badge--red'}`}>{n.status}</span>
                  </div>
                  <div className="broker-card-id" title={n.id}>{n.id.slice(0, 8)}…</div>
                  <div className="broker-card-meta">
                    <span>{n.ip}:{n.port}</span>
                    <span>hop: {n.hop_to_core}</span>
                  </div>
                </div>
              ))
              : <p className="empty">topology 수신 대기 중…</p>
            }
          </div>
        </section>

        {/* 실시간 이벤트 로그 */}
        <section className="event-log">
          <h2>Event Log ({filteredEvents.length}/{events.length})</h2>

          {/* 필터 컨트롤 */}
          <div className="event-filters">
            <select
              className="filter-select"
              value={filterType}
              onChange={e => setFilterType(e.target.value)}
            >
              {eventTypes.map(t => <option key={t} value={t}>{t === 'ALL' ? 'All types' : t}</option>)}
            </select>
            <select
              className="filter-select"
              value={filterBuilding}
              onChange={e => setFilterBuilding(e.target.value)}
            >
              {buildings.map(b => <option key={b} value={b}>{b === 'ALL' ? 'All buildings' : b}</option>)}
            </select>
            <button
              className={`filter-btn ${filterHighOnly ? 'filter-btn--active' : ''}`}
              onClick={() => setFilterHighOnly(v => !v)}
            >
              HIGH only
            </button>
            {(filterType !== 'ALL' || filterBuilding !== 'ALL' || filterHighOnly) && (
              <button
                className="filter-btn filter-btn--reset"
                onClick={() => { setFilterType('ALL'); setFilterBuilding('ALL'); setFilterHighOnly(false); }}
              >
                reset
              </button>
            )}
          </div>

          <ul className="event-list">
            {events.length === 0 && <li className="empty">이벤트 수신 대기 중…</li>}
            {events.length > 0 && filteredEvents.length === 0 && (
              <li className="empty">필터 조건에 맞는 이벤트 없음</li>
            )}
            {filteredEvents.map(e => (
              <li key={e.msg_id} className="event-item">
                <span className="event-time">{e.timestamp?.slice(11, 19) ?? '—'}</span>
                <span className={`badge ${EVENT_TYPE_COLOR[e.type] ?? 'badge--gray'}`}>{e.type}</span>
                {e.priority === 'HIGH' && <span className="badge badge--red">HIGH</span>}
                <span className="event-desc">
                  [{e.payload?.building_id ?? '?'}] {e.payload?.description ?? ''}
                </span>
              </li>
            ))}
          </ul>
        </section>

      </div>
    </div>
  );
}
