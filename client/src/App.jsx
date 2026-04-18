import { useEffect, useRef, useState } from 'react';
import { useMqtt } from './hooks/useMqtt.js';
import TopologyGraph from './components/TopologyGraph.jsx';
import {
  formatClockTime,
  formatDelayLabel,
  formatEventSourceOption,
  formatEventTime,
  getEventPresentation,
} from './mqtt/eventPresentation.js';
import { buildNodePresentationMap, getNodeAliasKey, resolveNodeAlias } from './mqtt/nodePresentation.js';
import './App.css';

const NODE_ALIAS_STORAGE_KEY = 'monitor-node-aliases';

function loadNodeAliases() {
  try {
    const raw = globalThis.localStorage?.getItem(NODE_ALIAS_STORAGE_KEY);
    if (!raw) return {};

    const parsed = JSON.parse(raw);
    return parsed && typeof parsed === 'object' ? parsed : {};
  } catch {
    return {};
  }
}

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
  BUFFERED_EVENTS: 'badge--yellow',
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

function formatNodeReference(nodeId, nodeDisplayMap, fallbackLabel = 'Unknown edge') {
  if (!nodeId) return fallbackLabel;

  const display = nodeDisplayMap?.get(nodeId);
  if (display?.listLabel) return display.listLabel;
  return `${nodeId.slice(0, 8)}…`;
}

function formatIncidentText(incident, nodeDisplayMap) {
  const shortId = incident.nodeId ? `${incident.nodeId.slice(0, 8)}…` : '(unknown)';
  const nodeRef = formatNodeReference(incident.nodeId, nodeDisplayMap);

  if (incident.type === 'CORE_DOWN') {
    return `${nodeRef} disconnected`;
  }
  if (incident.type === 'ACTIVE_CORE_DOWN') {
    return `${nodeRef} disconnected`;
  }
  if (incident.type === 'BACKUP_CORE_DOWN') {
    return `${nodeRef} disconnected`;
  }
  if (incident.type === 'FAILOVER_SWITCH') {
    return `Backup promoted to active${incident.endpoint ? ` (${incident.endpoint})` : ''}`;
  }
  if (incident.type === 'EDGE_DOWN') {
    return `${nodeRef} disconnected`;
  }
  if (incident.type === 'EDGE_UP') {
    return `${nodeRef} recovered`;
  }
  if (incident.type === 'BUFFERED_EVENTS') {
    const eventType = incident.eventType ? incident.eventType.replace('_', ' ') : 'events';
    return `${nodeRef} replayed queued ${eventType.toLowerCase()} (${formatDelayLabel(incident.delayMs)})`;
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
  const eventLogRef = useRef(null);
  const [nodeAliases, setNodeAliases] = useState(loadNodeAliases);
  const [isAliasModalOpen, setIsAliasModalOpen] = useState(false);
  const [aliasDrafts, setAliasDrafts] = useState({});
  const seenEdgeAliasKeysRef = useRef(new Set());

  useEffect(() => {
    setUrlInput(brokerUrl);
  }, [brokerUrl]);

  useEffect(() => {
    try {
      globalThis.localStorage?.setItem(NODE_ALIAS_STORAGE_KEY, JSON.stringify(nodeAliases));
    } catch {
      // ignore storage failures in private browsing / restricted environments
    }
  }, [nodeAliases]);

  // 노드 선택 state
  const [selectedNodeId, setSelectedNodeId] = useState(null);
  const selectedNode = topology?.nodes.find(n => n.id === selectedNodeId) ?? null;
  const nodeDisplayMap = buildNodePresentationMap(topology, nodeAliases);
  const selectedNodeDisplay = selectedNode ? nodeDisplayMap.get(selectedNode.id) ?? null : null;
  const edgeNodes = (topology?.nodes ?? [])
    .filter(node => node.role !== 'CORE')
    .sort((left, right) => {
      const leftNumber = nodeDisplayMap.get(left.id)?.edgeNumber ?? Number.MAX_SAFE_INTEGER;
      const rightNumber = nodeDisplayMap.get(right.id)?.edgeNumber ?? Number.MAX_SAFE_INTEGER;
      return leftNumber - rightNumber;
    });
  const edgeNodeIdsKey = edgeNodes.map(node => node.id).join('|');

  // 필터 state
  const [filterType,     setFilterType]     = useState('ALL');
  const [filterBuilding, setFilterBuilding] = useState('ALL');
  const [filterSourceId, setFilterSourceId] = useState('ALL');
  const [filterHighOnly, setFilterHighOnly] = useState(false);

  // 필터 옵션 (수신된 events에서 동적 추출)
  const nodeById = new Map((topology?.nodes ?? []).map(node => [node.id, node]));
  const eventTypes = ['ALL', ...new Set(events.map(e => e.type).filter(Boolean))];
  const buildings  = ['ALL', ...new Set(events.map(e => e.payload?.building_id).filter(Boolean))];
  const sourceNodes = ['ALL', ...new Set(events.map(e => e.source?.id).filter(Boolean))];

  // 필터 적용
  const filteredEvents = events.filter(e => {
    if (filterType     !== 'ALL' && e.type                  !== filterType)     return false;
    if (filterBuilding !== 'ALL' && e.payload?.building_id  !== filterBuilding) return false;
    if (filterSourceId !== 'ALL' && e.source?.id            !== filterSourceId) return false;
    if (filterHighOnly && e.priority !== 'HIGH')                                return false;
    return true;
  });

  // Stats (computed)
  const onlineCount   = topology?.nodes.filter(n => n.status === 'ONLINE').length  ?? 0;
  const offlineCount  = topology?.nodes.filter(n => n.status === 'OFFLINE').length ?? 0;
  const criticalCount = events.filter(e => e.priority === 'HIGH').length;
  const latestIncident = incidents[0] ?? null;

  function scrollToEventLog() {
    eventLogRef.current?.scrollIntoView({ behavior: 'smooth', block: 'start' });
  }

  function toggleCriticalFilter() {
    setFilterHighOnly(prev => !prev);
    scrollToEventLog();
  }

  function openAliasModal() {
    setAliasDrafts(() => {
      const nextDrafts = {};
      edgeNodes.forEach((node) => {
        nextDrafts[getNodeAliasKey(node)] = resolveNodeAlias(node, nodeAliases);
      });
      return nextDrafts;
    });
    setIsAliasModalOpen(true);
  }

  function closeAliasModal() {
    setIsAliasModalOpen(false);
  }

  function saveAliasModal() {
    setNodeAliases(prev => {
      const next = { ...prev };
      edgeNodes.forEach((node) => {
        const aliasKey = getNodeAliasKey(node);
        const alias = (aliasDrafts[aliasKey] ?? '').trim();
        delete next[node.id];
        if (alias) next[aliasKey] = alias;
        else delete next[aliasKey];
      });
      return next;
    });
    setIsAliasModalOpen(false);
  }

  useEffect(() => {
    if (!isAliasModalOpen) return;

    setAliasDrafts(prev => {
      const nextDrafts = {};
      edgeNodes.forEach((node) => {
        const aliasKey = getNodeAliasKey(node);
        nextDrafts[aliasKey] = prev[aliasKey] ?? resolveNodeAlias(node, nodeAliases);
      });
      return nextDrafts;
    });
  }, [isAliasModalOpen, edgeNodeIdsKey, nodeAliases]);

  useEffect(() => {
    if (edgeNodes.length === 0) return;

    const unseenKeys = edgeNodes
      .map(node => getNodeAliasKey(node))
      .filter(aliasKey => aliasKey && !seenEdgeAliasKeysRef.current.has(aliasKey));

    if (unseenKeys.length === 0) return;

    unseenKeys.forEach(aliasKey => seenEdgeAliasKeysRef.current.add(aliasKey));

    const hasUnnamedEdge = edgeNodes.some((node) => {
      const aliasKey = getNodeAliasKey(node);
      return unseenKeys.includes(aliasKey) && !resolveNodeAlias(node, nodeAliases);
    });

    if (hasUnnamedEdge) {
      openAliasModal();
    }
  }, [edgeNodeIdsKey, nodeAliases]);

  useEffect(() => {
    if (!isAliasModalOpen) return undefined;

    const handleKeyDown = (event) => {
      if (event.key === 'Escape') {
        setIsAliasModalOpen(false);
      }
    };

    globalThis.addEventListener?.('keydown', handleKeyDown);
    return () => globalThis.removeEventListener?.('keydown', handleKeyDown);
  }, [isAliasModalOpen]);

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
            placeholder="ws://<current-host>:9001"
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
        <button
          type="button"
          className={`stat-card stat-card--button ${filterHighOnly ? 'stat-card--active' : ''}`}
          onClick={toggleCriticalFilter}
          aria-pressed={filterHighOnly}
        >
          <span className="stat-value stat-value--orange">{criticalCount}</span>
          <span className="stat-label">Critical</span>
          <span className="stat-hint">{filterHighOnly ? 'Filtered' : 'Click to filter'}</span>
        </button>
      </div>

      {(latestIncident || incidents.length > 0) && (
        <section className="incident-section">
          <div className="incident-header">
            <h2>System Notices</h2>
            {latestIncident && (
              <span className="incident-summary">
                {formatIncidentText(latestIncident, nodeDisplayMap)} at {formatIncidentTime(latestIncident.ts)}
              </span>
            )}
          </div>
          <div className="incident-list">
            {incidents.slice(0, 5).map((incident) => (
              <div key={incident.key} className="incident-item">
                <span className={`badge ${INCIDENT_BADGE[incident.type] ?? 'badge--gray'}`}>
                  {incident.type.replace('_', ' ')}
                </span>
                <span className="incident-text">{formatIncidentText(incident, nodeDisplayMap)}</span>
                <span className="incident-time">{formatIncidentTime(incident.ts)}</span>
              </div>
            ))}
          </div>
        </section>
      )}

      {/* ── Cytoscape 토폴로지 그래프 ─────────────────────────── */}
      <section className="graph-section">
        <div className="section-header">
          <h2>Topology{selectedNode ? ` — ${selectedNodeDisplay?.listLabel ?? selectedNode.id.slice(0, 8)}` : ''}</h2>
          {edgeNodes.length > 0 && (
            <button type="button" className="section-btn" onClick={openAliasModal}>
              Name Edges
            </button>
          )}
        </div>
        <TopologyGraph topology={topology} onNodeClick={setSelectedNodeId} nodeDisplayMap={nodeDisplayMap} />

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
                {selectedNodeDisplay?.listLabel ?? selectedNode.id}
              </span>
              <button className="node-detail-close" onClick={() => setSelectedNodeId(null)}>✕</button>
            </div>
            <div className="node-detail-body">
              <span className="node-detail-kv"><span>UUID</span><code>{selectedNode.id}</code></span>
              {selectedNodeDisplay?.listLabel && (
                <span className="node-detail-kv"><span>Label</span><code>{selectedNodeDisplay.listLabel}</code></span>
              )}
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
                <button
                  type="button"
                  key={n.id}
                  className={`broker-card broker-card--button broker-card--${n.status.toLowerCase()} ${selectedNodeId === n.id ? 'broker-card--selected' : ''}`}
                  onClick={() => setSelectedNodeId(n.id)}
                >
                  <div className="broker-card-header">
                    <span className={`badge ${n.role === 'CORE' ? 'badge--purple' : 'badge--gray'}`}>{n.role}</span>
                    <span className={`badge ${n.status === 'ONLINE' ? 'badge--green' : 'badge--red'}`}>{n.status}</span>
                  </div>
                  <div className="broker-card-id" title={n.id}>
                    {nodeDisplayMap.get(n.id)?.listLabel ?? n.id.slice(0, 8)}
                  </div>
                  <div className="broker-card-meta">
                    <span>{n.ip}:{n.port}</span>
                    <span>hop: {n.hop_to_core}</span>
                  </div>
                </button>
              ))
              : <p className="empty">topology 수신 대기 중…</p>
            }
          </div>
        </section>

        {/* 실시간 이벤트 로그 */}
        <section ref={eventLogRef} className="event-log">
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
            <select
              className="filter-select"
              value={filterSourceId}
              onChange={e => setFilterSourceId(e.target.value)}
            >
              {sourceNodes.map(nodeId => (
                <option key={nodeId} value={nodeId}>
                  {nodeId === 'ALL' ? 'All edges' : formatEventSourceOption(nodeId, nodeById.get(nodeId), nodeDisplayMap)}
                </option>
              ))}
            </select>
            <button
              className={`filter-btn ${filterHighOnly ? 'filter-btn--active' : ''}`}
              onClick={() => setFilterHighOnly(v => !v)}
            >
              HIGH only
            </button>
            {(filterType !== 'ALL' || filterBuilding !== 'ALL' || filterSourceId !== 'ALL' || filterHighOnly) && (
              <button
                className="filter-btn filter-btn--reset"
                onClick={() => {
                  setFilterType('ALL');
                  setFilterBuilding('ALL');
                  setFilterSourceId('ALL');
                  setFilterHighOnly(false);
                }}
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
            {filteredEvents.map(e => {
              const eventView = getEventPresentation(e, nodeById, nodeDisplayMap);

              return (
                <li key={e.msg_id}>
                  <button
                    type="button"
                    className={`event-item event-item--${e.type?.toLowerCase() ?? 'unknown'} ${e._buffered ? 'event-item--buffered' : ''}`}
                    onClick={() => {
                      if (eventView.sourceId) setSelectedNodeId(eventView.sourceId);
                    }}
                  >
                    <div className="event-header">
                      <span className="event-time">{formatEventTime(e.timestamp)}</span>
                      <span className={`badge ${EVENT_TYPE_COLOR[e.type] ?? 'badge--gray'}`}>{e.type}</span>
                      {e.priority === 'HIGH' && <span className="badge badge--red">HIGH</span>}
                      {e._buffered && <span className="badge badge--purple">BUFFERED</span>}
                      <span className="event-source-chip">
                        {eventView.edgeLabel}
                      </span>
                    </div>

                    <div className="event-title-row">
                      <strong className="event-title">{eventView.sourceTitle}</strong>
                    </div>
                    <div className="event-location">{eventView.locationLabel}</div>

                    <div className="event-meta">
                      {eventView.sourceEndpoint && <span>{eventView.sourceEndpoint}</span>}
                      {e._buffered && <span>received {formatClockTime(e._receivedAt)}</span>}
                      {e._buffered && <span>{formatDelayLabel(e._delayMs)}</span>}
                      {eventView.descriptionLabel && <span>{eventView.descriptionLabel}</span>}
                    </div>
                  </button>
                </li>
              );
            })}
          </ul>
        </section>

      </div>

      {isAliasModalOpen && (
        <div className="modal-backdrop" onClick={closeAliasModal}>
          <div
            className="alias-modal"
            role="dialog"
            aria-modal="true"
            aria-labelledby="edge-alias-modal-title"
            onClick={event => event.stopPropagation()}
          >
            <div className="alias-modal-header">
              <div>
                <h2 id="edge-alias-modal-title">Edge Labels</h2>
                <p className="alias-modal-subtitle">
                  Give each edge a demo-friendly name. Edge numbers stay fixed and the labels will appear in the graph and event log.
                </p>
              </div>
              <button type="button" className="alias-modal-close" onClick={closeAliasModal}>✕</button>
            </div>

            <div className="alias-modal-list">
              {edgeNodes.map((node) => {
                const display = nodeDisplayMap.get(node.id);
                return (
                  <label key={node.id} className="alias-modal-row">
                    <div className="alias-modal-row-head">
                      <span className="badge badge--purple">{display?.edgeLabel ?? 'EDGE'}</span>
                      <strong>{display?.alias || display?.endpoint || node.ip || node.id.slice(0, 8)}</strong>
                    </div>
                    <span className="alias-modal-row-meta">
                      {node.ip}:{node.port} · {node.id.slice(0, 8)}…
                    </span>
                    <input
                      className="alias-modal-input"
                      value={aliasDrafts[getNodeAliasKey(node)] ?? ''}
                      onChange={event => {
                        const value = event.target.value;
                        const aliasKey = getNodeAliasKey(node);
                        setAliasDrafts(prev => ({ ...prev, [aliasKey]: value }));
                      }}
                      placeholder="예: 공학관 1층"
                    />
                  </label>
                );
              })}
            </div>

            <div className="alias-modal-actions">
              <button type="button" className="section-btn section-btn--ghost" onClick={closeAliasModal}>
                Later
              </button>
              <button type="button" className="section-btn" onClick={saveAliasModal}>
                Save Labels
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
