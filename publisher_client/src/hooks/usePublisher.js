import { useCallback, useEffect, useRef, useState } from 'react';
import mqtt from 'mqtt';
import { buildMessage, buildTopic } from '../mqtt/message.js';
import { parseBrokerCandidates } from '../mqtt/failover.js';
import {
  findPreferredCoreBroker,
  selectFallbackBroker,
  shouldAcceptCtUpdate,
  shouldReturnToPrimary,
  resolvePrimaryEdgeId,
} from '../mqtt/ctFailover.js';
import { generateUuid } from '../utils/uuid.js';

const MAX_LOG = 30;
const MAX_QUEUE = 100;
const CLIENT_ID = `pub-${generateUuid().slice(0, 8)}`;
const RECONNECT_DELAY_MS = 1500;
const FALLBACK_RETRY_DELAY_MS = 3000;
const CT_TOPIC = 'campus/monitor/topology';

function trimEventLog(events) {
  return events.slice(0, MAX_LOG);
}

export function usePublisher() {
  const [status, setStatus] = useState('disconnected');
  const [events, setEvents] = useState([]);
  const [activeBrokerUrl, setActiveBrokerUrl] = useState('');
  const [queueSize, setQueueSize] = useState(0);
  const [ctReceived, setCtReceived] = useState(false);
  const [onFallback, setOnFallback] = useState(false);

  const clientRef = useRef(null);
  const reconnectTimerRef = useRef(null);
  const manualDisconnectRef = useRef(false);
  const queueRef = useRef([]);
  const activeBrokerUrlRef = useRef('');
  const cachedCoreUrlRef = useRef('');
  const wsPortRef = useRef(9001);
  const wsProtocolRef = useRef('ws:');

  // CT 기반 failover 상태
  const ctRef = useRef(null);
  const primaryEdgeIdRef = useRef(null);
  const primaryBrokerUrlRef = useRef('');
  const primaryIpRef = useRef('');
  const onFallbackRef = useRef(false);

  const setQueueSizeFromRef = useCallback(() => {
    setQueueSize(queueRef.current.length);
  }, []);

  const upsertEvent = useCallback((entry) => {
    setEvents(prev => trimEventLog([entry, ...prev.filter(item => item.msg_id !== entry.msg_id)]));
  }, []);

  const clearReconnectTimer = useCallback(() => {
    if (!reconnectTimerRef.current) return;
    clearTimeout(reconnectTimerRef.current);
    reconnectTimerRef.current = null;
  }, []);

  const destroyClient = useCallback((force = true) => {
    clearReconnectTimer();
    const client = clientRef.current;
    clientRef.current = null;
    if (!client) return;
    client.removeAllListeners();
    client.end(force);
  }, [clearReconnectTimer]);

  const buildBrokerUrl = useCallback((host) => {
    if (!host) return '';
    return `${wsProtocolRef.current}//${host}:${wsPortRef.current}`;
  }, []);

  const isDeliveringViaFallback = useCallback(() => (
    onFallbackRef.current ||
    (
      activeBrokerUrlRef.current &&
      primaryBrokerUrlRef.current &&
      activeBrokerUrlRef.current !== primaryBrokerUrlRef.current
    )
  ), []);

  const rememberPreferredCore = useCallback((ct) => {
    const preferredCore = findPreferredCoreBroker(ct, primaryEdgeIdRef.current);
    if (!preferredCore?.ip) return;
    cachedCoreUrlRef.current = buildBrokerUrl(preferredCore.ip);
  }, [buildBrokerUrl]);

  const applyOriginRoute = useCallback((msg) => {
    if (!msg?.route || !primaryEdgeIdRef.current) return msg;
    msg.route.original_node = primaryEdgeIdRef.current;
    return msg;
  }, []);

  const flushQueue = useCallback(() => {
    const client = clientRef.current;
    if (!client?.connected || queueRef.current.length === 0) return;

    const queuedItems = [...queueRef.current];
    queueRef.current = [];
    setQueueSizeFromRef();

    queuedItems.forEach((item) => {
      let sendPayload = item.payload;
      try {
        const parsedMsg = JSON.parse(item.payload);
        applyOriginRoute(parsedMsg);
        parsedMsg.was_queued = true;
        if (isDeliveringViaFallback()) {
          parsedMsg.via_failover = true;
          parsedMsg.intended_edge_ip = primaryIpRef.current;
        }
        sendPayload = JSON.stringify(parsedMsg);
      } catch {
        // keep original payload
      }

      client.publish(item.topic, sendPayload, { qos: 1 }, (error) => {
        if (error) {
          queueRef.current.unshift(item);
          setQueueSizeFromRef();
          upsertEvent({ ...item.log, status: 'queued', brokerUrl: activeBrokerUrlRef.current, sentAt: item.sentAt });
          return;
        }
        upsertEvent({
          ...item.log,
          status: item.log.status === 'queued' ? 'flushed' : 'sent',
          brokerUrl: activeBrokerUrlRef.current,
          sentAt: Date.now(),
        });
      });
    });
  }, [applyOriginRoute, isDeliveringViaFallback, setQueueSizeFromRef, upsertEvent]);

  // 단일 URL로 MQTT 연결 + CT 구독
  const connectToBroker = useCallback((brokerUrl) => {
    if (!brokerUrl) return false;

    destroyClient(true);
    activeBrokerUrlRef.current = brokerUrl;
    setActiveBrokerUrl(brokerUrl);
    setStatus('connecting');

    const client = mqtt.connect(brokerUrl, {
      clientId: CLIENT_ID,
      reconnectPeriod: 0,
      connectTimeout: 5000,
    });
    clientRef.current = client;

    client.on('connect', () => {
      if (clientRef.current !== client) return;
      setStatus('connected');
      client.subscribe(CT_TOPIC, { qos: 1 });
      flushQueue();
    });

    client.on('message', (topic, payload) => {
      if (clientRef.current !== client) return;
      if (topic === CT_TOPIC) handleCtMessage(payload.toString()); // eslint-disable-line no-use-before-define
    });

    client.on('close', () => {
      if (clientRef.current !== client) return;
      if (manualDisconnectRef.current) {
        setStatus('disconnected');
        return;
      }
      setStatus('disconnected');
      clearReconnectTimer();
      reconnectTimerRef.current = setTimeout(() => {
        reconnectTimerRef.current = null;
        attemptFailover(); // eslint-disable-line no-use-before-define
      }, RECONNECT_DELAY_MS);
    });

    client.on('error', () => {
      if (clientRef.current !== client) return;
      setStatus('error');
      client.end(true);
    });

    return true;
  }, [clearReconnectTimer, destroyClient, flushQueue]);

  const schedulePrimaryRetry = useCallback(() => {
    clearReconnectTimer();
    reconnectTimerRef.current = setTimeout(() => {
      reconnectTimerRef.current = null;
      if (!manualDisconnectRef.current) {
        connectToBroker(primaryBrokerUrlRef.current);
      }
    }, FALLBACK_RETRY_DELAY_MS);
  }, [clearReconnectTimer, connectToBroker]);

  // CT 기반 Failover: 최적 대체 브로커로 재연결
  const attemptFailover = useCallback(() => {
    if (manualDisconnectRef.current) return false;

    const ct = ctRef.current;
    if (!ct || !Array.isArray(ct.nodes) || ct.nodes.length === 0) {
      if (cachedCoreUrlRef.current) {
        const fallbackUrl = cachedCoreUrlRef.current;
        const isPrimaryTarget = fallbackUrl === primaryBrokerUrlRef.current;
        onFallbackRef.current = !isPrimaryTarget;
        setOnFallback(!isPrimaryTarget);
        connectToBroker(fallbackUrl);
        return true;
      }

      schedulePrimaryRetry();
      return false;
    }

    const fallback = selectFallbackBroker(ct, primaryEdgeIdRef.current);
    if (!fallback.found) {
      schedulePrimaryRetry();
      return false;
    }

    const fallbackUrl = buildBrokerUrl(fallback.ip);
    const isPrimaryTarget = fallbackUrl === primaryBrokerUrlRef.current;
    onFallbackRef.current = !isPrimaryTarget;
    setOnFallback(!isPrimaryTarget);
    connectToBroker(fallbackUrl);
    return true;
  }, [buildBrokerUrl, connectToBroker, schedulePrimaryRetry]);

  // CT 메시지 처리: primaryEdgeId 확정 + failover/return-to-primary 판단
  const handleCtMessage = useCallback((jsonStr) => {
    let ct;
    try {
      ct = JSON.parse(jsonStr);
    } catch {
      return;
    }

    if (!shouldAcceptCtUpdate(ctRef.current, ct)) return;

    ctRef.current = ct;
    setCtReceived(true);
    rememberPreferredCore(ct);

    if (primaryIpRef.current) {
      const resolvedPrimaryEdgeId = resolvePrimaryEdgeId(ct, primaryIpRef.current);
      if (resolvedPrimaryEdgeId) {
        primaryEdgeIdRef.current = resolvedPrimaryEdgeId;
      }
    }

    if (onFallbackRef.current && primaryEdgeIdRef.current) {
      if (shouldReturnToPrimary(ct, primaryEdgeIdRef.current)) {
        onFallbackRef.current = false;
        setOnFallback(false);
        connectToBroker(primaryBrokerUrlRef.current);
        return;
      }
    }

    if (!onFallbackRef.current && primaryEdgeIdRef.current) {
      const primaryNode = ct.nodes.find(n => n.id === primaryEdgeIdRef.current);
      if (!primaryNode || primaryNode.status === 'OFFLINE') {
        attemptFailover();
      }
    }
  }, [attemptFailover, connectToBroker, rememberPreferredCore]);

  const connect = useCallback((rawBrokerInput) => {
    const brokers = parseBrokerCandidates(rawBrokerInput);
    manualDisconnectRef.current = false;

    if (brokers.length === 0) {
      destroyClient(true);
      setStatus('error');
      return false;
    }

    // CT 기반 failover: 첫 번째 URL만 primary로 사용
    const primaryUrl = brokers[0];
    let hostname = 'localhost';
    let port = 9001;
    let protocol = 'ws:';

    try {
      const parsed = new URL(primaryUrl);
      hostname = parsed.hostname;
      port = parseInt(parsed.port, 10) || 9001;
      protocol = parsed.protocol === 'wss:' ? 'wss:' : 'ws:';
    } catch {
      // 잘못된 URL은 parseBrokerCandidates가 이미 걸러냄
    }

    primaryBrokerUrlRef.current = primaryUrl;
    primaryIpRef.current = hostname;
    wsPortRef.current = port;
    wsProtocolRef.current = protocol;
    cachedCoreUrlRef.current = '';
    onFallbackRef.current = false;
    primaryEdgeIdRef.current = null;
    ctRef.current = null;
    setCtReceived(false);
    setOnFallback(false);

    return connectToBroker(primaryUrl);
  }, [connectToBroker, destroyClient]);

  const disconnect = useCallback(() => {
    manualDisconnectRef.current = true;
    primaryBrokerUrlRef.current = '';
    primaryIpRef.current = '';
    cachedCoreUrlRef.current = '';
    onFallbackRef.current = false;
    ctRef.current = null;
    activeBrokerUrlRef.current = '';
    setActiveBrokerUrl('');
    setCtReceived(false);
    setOnFallback(false);
    destroyClient(true);
    setStatus('disconnected');
  }, [destroyClient]);

  const queuePublish = useCallback((entry) => {
    if (queueRef.current.length >= MAX_QUEUE) {
      queueRef.current.shift();
    }
    queueRef.current.push(entry);
    setQueueSizeFromRef();
  }, [setQueueSizeFromRef]);

  const publish = useCallback(({ publisherId, type, buildingId, cameraId, description = '' }) => {
    let msg;
    let topic;

    try {
      msg = buildMessage({ publisherId, type, buildingId, cameraId, description, qos: 1 });
      applyOriginRoute(msg);
      topic = buildTopic(type, buildingId, cameraId);
    } catch {
      return false;
    }

    if (!topic) return false;

    if (isDeliveringViaFallback()) {
      msg.via_failover = true;
      msg.intended_edge_ip = primaryIpRef.current;
    }

    const payload = JSON.stringify(msg);
    const baseLog = {
      ...msg,
      topic,
      brokerUrl: activeBrokerUrlRef.current,
      sentAt: Date.now(),
    };
    const queueEntry = { topic, payload, sentAt: baseLog.sentAt, log: baseLog };

    const client = clientRef.current;
    if (!client?.connected) {
      queuePublish(queueEntry);
      upsertEvent({ ...baseLog, status: 'queued' });

      if (!manualDisconnectRef.current && status !== 'connecting') {
        attemptFailover();
      }
      return false;
    }

    client.publish(topic, payload, { qos: 1 }, (error) => {
      if (error) {
        queuePublish(queueEntry);
        upsertEvent({ ...baseLog, status: 'queued' });
        if (!manualDisconnectRef.current && status !== 'connecting') {
          attemptFailover();
        }
        return;
      }
      upsertEvent({ ...baseLog, status: 'sent', brokerUrl: activeBrokerUrlRef.current, sentAt: Date.now() });
    });

    return true;
  }, [applyOriginRoute, attemptFailover, isDeliveringViaFallback, queuePublish, status, upsertEvent]);

  useEffect(() => () => {
    manualDisconnectRef.current = true;
    destroyClient(true);
  }, [destroyClient]);

  return {
    status,
    events,
    activeBrokerUrl,
    queueSize,
    ctReceived,
    onFallback,
    connect,
    disconnect,
    publish,
  };
}
