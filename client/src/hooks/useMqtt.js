import { useState, useEffect, useRef } from 'react';
import mqtt from 'mqtt';
import { parseConnectionTable, parseMqttMessage } from '../mqtt/parsers.js';
import { buildPresentationTopology, reconcileHiddenNodeIds } from '../mqtt/topologyVisibility.js';
import {
  buildBrokerUrl,
  parseBrokerEndpoint,
  resolveInitialBrokerUrl,
  resolveBackupReconnectTarget,
  selectPromotedActiveNode,
} from '../mqtt/failover.js';

const MAX_EVENTS = 50;
const MAX_ALERTS = 20;
const ALERT_TTL_MS = 5000;

/**
 * MQTT WebSocket 연결 + 전체 토픽 구독 + JSON 파싱 + Core 재연결
 *
 * @returns {{
 *   status: 'connecting'|'connected'|'disconnected'|'error',
 *   topology: object|null,
 *   events: object[],
 *   alerts: object[],
 * }}
 */
export function useMqtt() {
  const [brokerUrl, setBrokerUrl]       = useState(() => (
    resolveInitialBrokerUrl(import.meta.env.VITE_MQTT_URL, globalThis.location)
  ));
  const [status, setStatus]             = useState('connecting');
  const [topology, setTopology]         = useState(null);
  const [events, setEvents]             = useState([]);
  const [alerts, setAlerts]             = useState([]);
  const [incidents, setIncidents]       = useState([]);
  const [reconnectInfo, setReconnectInfo] = useState(null); // { url, reason } | null

  const clientRef      = useRef(null);
  const seenMsgIds     = useRef(new Set());
  // node_down/node_up 알림 중복 제거용: "topic:ct.version" 형태 키
  const seenAlertKeys  = useRef(new Set());
  const seenIncidentKeysRef = useRef(new Set());
  // raw topology와 발표용 topology를 분리해서 보관
  const rawTopologyRef     = useRef(null);
  const topologyRef        = useRef(null);
  const hiddenNodeIdsRef   = useRef(new Set());
  // failover 이벤트(LWT/core_switch) 수신 시 true → 다음 CT 한 번은 버전 가드 무시
  const forceAcceptNextRef = useRef(false);

  useEffect(() => {
    const pushIncident = (incident) => {
      if (!incident?.key || seenIncidentKeysRef.current.has(incident.key)) return;
      seenIncidentKeysRef.current.add(incident.key);
      setIncidents(prev => [incident, ...prev].slice(0, 12));
    };

    const reconnectToBrokerHost = (nextHost, reason) => {
      const nextUrl = buildBrokerUrl(brokerUrl, nextHost);
      if (!nextUrl || nextUrl === brokerUrl) return false;

      setReconnectInfo({ url: nextUrl, reason });
      setBrokerUrl(nextUrl);
      return true;
    };

    const applyPresentationTopology = (rawTopology) => {
      rawTopologyRef.current = rawTopology;
      hiddenNodeIdsRef.current = reconcileHiddenNodeIds(rawTopology, hiddenNodeIdsRef.current);
      const nextTopology = buildPresentationTopology(rawTopology, hiddenNodeIdsRef.current);
      topologyRef.current = nextTopology;
      setTopology(nextTopology);
    };

    const refreshVisibleTopology = () => {
      if (!rawTopologyRef.current) return;
      hiddenNodeIdsRef.current = reconcileHiddenNodeIds(rawTopologyRef.current, hiddenNodeIdsRef.current);
      const nextTopology = buildPresentationTopology(rawTopologyRef.current, hiddenNodeIdsRef.current);
      topologyRef.current = nextTopology;
      setTopology(nextTopology);
    };

    const client = mqtt.connect(brokerUrl, {
      clientId: `monitor-${Math.random().toString(16).slice(2, 8)}`,
      clean: true,
      reconnectPeriod: 3000,
    });
    clientRef.current = client;

    client.on('connect', () => {
      setStatus('connected');
      setReconnectInfo(null);
      client.subscribe([
        'campus/monitor/topology',   // M-04: Connection Table 브로드캐스트
        'campus/data/+',             // D-01: 이벤트 로그
        'campus/data/+/+',           // D-02: 건물별 세분화 이벤트
        'campus/alert/node_down/+',  // A-01: Node OFFLINE 알림
        'campus/alert/node_up/+',    // A-02: Node 복구 알림
        'campus/alert/core_switch',  // A-03: Active Core 변경 → 재연결
        'campus/will/core/+',        // W-01: Core LWT (비정상 종료) → 재연결
      ], { qos: 1 });
    });

    client.on('message', (topic, payload) => {
      const raw = payload.toString();

      // ── M-04: Connection Table ──────────────────────────────────────
      if (topic === 'campus/monitor/topology') {
        const parsed = parseConnectionTable(raw);
        if (!parsed) return;

        const previousTopology = rawTopologyRef.current;
        if (!forceAcceptNextRef.current && previousTopology && parsed.version <= previousTopology.version) {
          return;
        }

        forceAcceptNextRef.current = false;
        applyPresentationTopology(parsed);

        const promotedNode = selectPromotedActiveNode(previousTopology, parsed, brokerUrl);
        if (promotedNode?.ip) {
          reconnectToBrokerHost(promotedNode.ip, 'M-04');
        }
        return;
      }

      // ── D-01 / D-02: CCTV 이벤트 로그 ─────────────────────────────
      if (topic.startsWith('campus/data/')) {
        const parsed = parseMqttMessage(raw);
        if (!parsed) return;
        // msg_id 중복 제거
        if (seenMsgIds.current.has(parsed.msg_id)) return;
        seenMsgIds.current.add(parsed.msg_id);
        setEvents(prev => [{ ...parsed, _topic: topic }, ...prev].slice(0, MAX_EVENTS));
        return;
      }

      // ── A-01 / A-02: Node 상태 알림 ────────────────────────────────
      // core/main.cpp 는 node_down 토픽에 ConnectionTable JSON을 publish함
      if (topic.startsWith('campus/alert/node_down/') ||
          topic.startsWith('campus/alert/node_up/')) {
        const ct = parseConnectionTable(raw);
        const nodeId = topic.split('/').pop();
        const nodeInCt = ct?.nodes?.find((node) => node.id === nodeId) ?? null;
        const nodeIsOfflineInCt = nodeInCt ? nodeInCt.status === 'OFFLINE' : true;
        if (topic.startsWith('campus/alert/node_down/')) {
          if (nodeIsOfflineInCt) {
            pushIncident({
              key: `node_down:${nodeId}:${ct?.version ?? 'na'}`,
              type: 'EDGE_DOWN',
              role: 'NODE',
              nodeId,
              ts: Date.now(),
            });
          }
        } else {
          pushIncident({
            key: `node_up:${nodeId}:${ct?.version ?? 'na'}`,
            type: 'EDGE_UP',
            role: 'NODE',
            nodeId,
            ts: Date.now(),
          });
        }
        // CT 버전 기반 중복 제거 (QoS 1 재전달 대응)
        const alertKey = `${topic}:${ct?.version ?? raw.slice(0, 32)}`;
        if (seenAlertKeys.current.has(alertKey)) return;
        seenAlertKeys.current.add(alertKey);
        // CT가 있으면 topology도 버전 가드 후 갱신
        if (ct && (!rawTopologyRef.current || ct.version > rawTopologyRef.current.version)) {
          applyPresentationTopology(ct);
        } else {
          refreshVisibleTopology();
        }
        const alert = { topic, nodeId, ct, raw, ts: Date.now() };
        setAlerts(prev => [alert, ...prev].slice(0, MAX_ALERTS));
        // ALERT_TTL_MS 후 자동 제거
        setTimeout(() => {
          setAlerts(prev => prev.filter(a => a.ts !== alert.ts));
        }, ALERT_TTL_MS);
        return;
      }

      // ── A-03: Active Core 교체 알림 → backup Core로 재연결 ──────────
      if (topic === 'campus/alert/core_switch') {
        const parsed = parseMqttMessage(raw);
        // msg_id 기반 중복 제거 (QoS 1 재전달 대응)
        if (parsed) {
          if (seenMsgIds.current.has(parsed.msg_id)) return;
          seenMsgIds.current.add(parsed.msg_id);
        }
        const alert = { topic, msg: parsed, raw, ts: Date.now() };
        setAlerts(prev => [alert, ...prev].slice(0, MAX_ALERTS));
        setTimeout(() => {
          setAlerts(prev => prev.filter(a => a.ts !== alert.ts));
        }, ALERT_TTL_MS);
        pushIncident({
          key: `core_switch:${parsed?.source?.id ?? parsed?.msg_id ?? alert.ts}`,
          type: 'FAILOVER_SWITCH',
          role: 'CORE',
          nodeId: parsed?.source?.id ?? '',
          endpoint: parsed?.payload?.description ?? '',
          ts: alert.ts,
        });
        forceAcceptNextRef.current = true;

        const nextEndpoint = parseBrokerEndpoint(parsed?.payload?.description ?? '');
        if (nextEndpoint?.host) {
          reconnectToBrokerHost(nextEndpoint.host, 'core_switch');
        }
        return;
      }

      // ── W-01: Core LWT (비정상 종료) → backup Core로 재연결 ─────────
      if (topic.startsWith('campus/will/core/')) {
        const parsed = parseMqttMessage(raw);
        const failedCoreId = topic.slice('campus/will/core/'.length);
        const previousTopology = rawTopologyRef.current;
        let incidentType = 'CORE_DOWN';
        if (previousTopology?.active_core_id === failedCoreId) {
          incidentType = 'ACTIVE_CORE_DOWN';
        } else if (previousTopology?.backup_core_id === failedCoreId) {
          incidentType = 'BACKUP_CORE_DOWN';
        }
        hiddenNodeIdsRef.current.add(failedCoreId);
        refreshVisibleTopology();
        pushIncident({
          key: `core_down:${failedCoreId}`,
          type: incidentType,
          role: 'CORE',
          nodeId: failedCoreId,
          ts: Date.now(),
        });
        // msg_id 기반 중복 제거 (QoS 1 재전달 대응)
        if (parsed) {
          if (seenMsgIds.current.has(parsed.msg_id)) return;
          seenMsgIds.current.add(parsed.msg_id);
        }
        const alert = { topic, msg: parsed, raw, ts: Date.now() };
        setAlerts(prev => [alert, ...prev].slice(0, MAX_ALERTS));
        setTimeout(() => {
          setAlerts(prev => prev.filter(a => a.ts !== alert.ts));
        }, ALERT_TTL_MS);
        forceAcceptNextRef.current = true;

        if (incidentType === 'ACTIVE_CORE_DOWN') {
          const reconnectNode = resolveBackupReconnectTarget(previousTopology);
          if (reconnectNode?.ip) {
            reconnectToBrokerHost(reconnectNode.ip, 'W-01');
          }
        }
        return;
      }
    });

    client.on('error',     () => setStatus('error'));
    client.on('close',     () => setStatus('disconnected'));
    client.on('reconnect', () => setStatus('connecting'));

    return () => { client.end(true); };
  }, [brokerUrl]); // brokerUrl 변경 시 재연결

  return { status, topology, events, alerts, incidents, reconnectInfo, brokerUrl, setBrokerUrl };
}
