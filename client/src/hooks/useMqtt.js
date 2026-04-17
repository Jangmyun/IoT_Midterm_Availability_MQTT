import { useState, useEffect, useRef } from 'react';
import mqtt from 'mqtt';
import { parseConnectionTable, parseMqttMessage } from '../mqtt/parsers.js';

const DEFAULT_URL = import.meta.env.VITE_MQTT_URL ?? 'ws://localhost:9001';
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
  const [brokerUrl, setBrokerUrl]       = useState(DEFAULT_URL);
  const [status, setStatus]             = useState('connecting');
  const [topology, setTopology]         = useState(null);
  const [events, setEvents]             = useState([]);
  const [alerts, setAlerts]             = useState([]);
  const [reconnectInfo, setReconnectInfo] = useState(null); // { url, reason } | null

  const clientRef      = useRef(null);
  const seenMsgIds     = useRef(new Set());
  // node_down/node_up 알림 중복 제거용: "topic:ct.version" 형태 키
  const seenAlertKeys  = useRef(new Set());
  // topology를 ref로도 보관 — message 핸들러가 클로저 내에서 최신값을 참조해야 함
  const topologyRef        = useRef(null);
  // failover 이벤트(LWT/core_switch) 수신 시 true → 다음 CT 한 번은 버전 가드 무시
  const forceAcceptNextRef = useRef(false);

  useEffect(() => {
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
        setTopology(prev => {
          // version guard: 구버전 CT는 무시. failover 이벤트 후 한 번은 강제 수락
          if (!forceAcceptNextRef.current && prev && parsed.version <= prev.version) return prev;
          forceAcceptNextRef.current = false;
          topologyRef.current = parsed;
          return parsed;
        });
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
        // CT 버전 기반 중복 제거 (QoS 1 재전달 대응)
        const alertKey = `${topic}:${ct?.version ?? raw.slice(0, 32)}`;
        if (seenAlertKeys.current.has(alertKey)) return;
        seenAlertKeys.current.add(alertKey);
        // CT가 있으면 topology도 버전 가드 후 갱신
        if (ct) {
          setTopology(prev => {
            if (prev && ct.version <= prev.version) return prev;
            topologyRef.current = ct;
            return ct;
          });
        }
        const nodeId = topic.split('/').pop();
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
        forceAcceptNextRef.current = true;
        reconnectToBackup('A-03');
        return;
      }

      // ── W-01: Core LWT (비정상 종료) → backup Core로 재연결 ─────────
      if (topic.startsWith('campus/will/core/')) {
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
        forceAcceptNextRef.current = true;
        reconnectToBackup('W-01');
        return;
      }
    });

    client.on('error',     () => setStatus('error'));
    client.on('close',     () => setStatus('disconnected'));
    client.on('reconnect', () => setStatus('connecting'));

    return () => { client.end(true); };
  }, [brokerUrl]); // brokerUrl 변경 시 재연결

  /**
   * topology의 backup_core_id 노드 IP:Port로 재연결.
   * topology를 ref로 참조해 클로저 stale 문제를 방지.
   * @param {'W-01'|'A-03'} reason - 재연결 트리거 원인
   */
  function reconnectToBackup(reason) {
    const ct = topologyRef.current;
    if (!ct) return;

    const backupNode = ct.nodes.find(n => n.id === ct.backup_core_id);
    if (!backupNode) return;

    const newUrl = `ws://${backupNode.ip}:${backupNode.port}`;
    if (newUrl === brokerUrl) return; // 이미 같은 주소면 무시

    setReconnectInfo({ url: newUrl, reason });
    clientRef.current?.end(true);
    setBrokerUrl(newUrl); // state 변경 → useEffect 재실행 → 재연결
  }

  return { status, topology, events, alerts, reconnectInfo, brokerUrl, setBrokerUrl };
}
