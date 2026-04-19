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
const KEEPALIVE_SECONDS = 5;
const HEALTH_CHECK_INTERVAL_MS = 1000;
const PUBLISH_ACK_TIMEOUT_MS = 1500;
const DUPLICATE_PUBLISH_WINDOW_MS = 150;
const CT_TOPIC = 'campus/monitor/topology';

function trimEventLog(events) {
  return events.slice(0, MAX_LOG);
}

export function buildPublishAttemptKey({ type, buildingId, cameraId, description = '' }) {
  return [type, buildingId, cameraId, description].join('\u0001');
}

export function shouldSuppressDuplicatePublish(
  previousAttempt,
  nextKey,
  now = Date.now(),
  windowMs = DUPLICATE_PUBLISH_WINDOW_MS,
) {
  if (!previousAttempt || !nextKey) return false;
  return previousAttempt.key === nextKey && (now - previousAttempt.at) < windowMs;
}

export function isBrokerActivityStale(
  lastBrokerActivityAt,
  now = Date.now(),
  keepaliveSeconds = KEEPALIVE_SECONDS,
) {
  if (!lastBrokerActivityAt || keepaliveSeconds <= 0) return false;
  const staleThresholdMs = Math.ceil((keepaliveSeconds * 1000 * 3) / 2) + 250;
  return (now - lastBrokerActivityAt) > staleThresholdMs;
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
  const healthCheckTimerRef = useRef(null);
  const manualDisconnectRef = useRef(false);
  const queueRef = useRef([]);
  const pendingPublishesRef = useRef(new Map());
  const lastPublishAttemptRef = useRef(null);
  const activeBrokerUrlRef = useRef('');
  const cachedCoreUrlRef = useRef('');
  const lastBrokerActivityAtRef = useRef(0);
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

  const clearHealthCheckTimer = useCallback(() => {
    if (!healthCheckTimerRef.current) return;
    clearInterval(healthCheckTimerRef.current);
    healthCheckTimerRef.current = null;
  }, []);

  const queuePublish = useCallback((entry) => {
    if (queueRef.current.some(item => item.log.msg_id === entry.log.msg_id)) {
      return false;
    }

    if (queueRef.current.length >= MAX_QUEUE) {
      queueRef.current.shift();
    }
    queueRef.current.push(entry);
    setQueueSizeFromRef();
    return true;
  }, [setQueueSizeFromRef]);

  const clearPendingPublishes = useCallback((mode = 'discard') => {
    const entries = [...pendingPublishesRef.current.values()];
    pendingPublishesRef.current.clear();

    entries.forEach((entry) => {
      clearTimeout(entry.timeoutId);
      if (mode !== 'queue' || entry.settled) return;
      if (!entry.queued) {
        entry.queued = queuePublish(entry.queueEntry) || entry.queued;
        upsertEvent({
          ...entry.queueEntry.log,
          status: 'queued',
          brokerUrl: activeBrokerUrlRef.current || entry.queueEntry.log.brokerUrl,
          sentAt: entry.queueEntry.sentAt,
        });
      }
    });
  }, [queuePublish, upsertEvent]);

  const markBrokerActivity = useCallback(() => {
    lastBrokerActivityAtRef.current = Date.now();
  }, []);

  const destroyClient = useCallback((force = true) => {
    clearReconnectTimer();
    clearHealthCheckTimer();
    const client = clientRef.current;
    clientRef.current = null;
    if (!client) return;
    client.removeAllListeners();
    client.end(force);
  }, [clearHealthCheckTimer, clearReconnectTimer]);

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
        markBrokerActivity();
        upsertEvent({
          ...item.log,
          status: item.log.status === 'queued' ? 'flushed' : 'sent',
          brokerUrl: activeBrokerUrlRef.current,
          sentAt: Date.now(),
        });
      });
    });
  }, [applyOriginRoute, isDeliveringViaFallback, markBrokerActivity, setQueueSizeFromRef, upsertEvent]);

  const scheduleReconnect = useCallback((delayMs = RECONNECT_DELAY_MS) => {
    clearReconnectTimer();
    reconnectTimerRef.current = setTimeout(() => {
      reconnectTimerRef.current = null;
      attemptFailover(); // eslint-disable-line no-use-before-define
    }, delayMs);
  }, [clearReconnectTimer]);

  const handleConnectionLoss = useCallback((delayMs = RECONNECT_DELAY_MS) => {
    if (manualDisconnectRef.current) {
      setStatus('disconnected');
      return false;
    }

    clearPendingPublishes('queue');
    activeBrokerUrlRef.current = '';
    setActiveBrokerUrl('');
    setStatus('disconnected');
    destroyClient(true);
    scheduleReconnect(delayMs);
    return true;
  }, [clearPendingPublishes, destroyClient, scheduleReconnect]);

  const startHealthCheck = useCallback(() => {
    clearHealthCheckTimer();
    healthCheckTimerRef.current = setInterval(() => {
      const client = clientRef.current;
      if (!client?.connected || manualDisconnectRef.current) return;
      if (!isBrokerActivityStale(lastBrokerActivityAtRef.current)) return;
      handleConnectionLoss(0);
    }, HEALTH_CHECK_INTERVAL_MS);
  }, [clearHealthCheckTimer, handleConnectionLoss]);

  // 단일 URL로 MQTT 연결 + CT 구독
  const connectToBroker = useCallback((brokerUrl) => {
    if (!brokerUrl) return false;

    destroyClient(true);
    activeBrokerUrlRef.current = brokerUrl;
    setActiveBrokerUrl(brokerUrl);
    setStatus('connecting');

    const client = mqtt.connect(brokerUrl, {
      clientId: CLIENT_ID,
      clean: true,
      keepalive: KEEPALIVE_SECONDS,
      reconnectPeriod: 0,
      connectTimeout: 5000,
    });
    clientRef.current = client;

    client.on('connect', () => {
      if (clientRef.current !== client) return;
      markBrokerActivity();
      setStatus('connected');
      client.subscribe(CT_TOPIC, { qos: 1 });
      startHealthCheck();
      flushQueue();
    });

    client.on('message', (topic, payload) => {
      if (clientRef.current !== client) return;
      markBrokerActivity();
      if (topic === CT_TOPIC) handleCtMessage(payload.toString()); // eslint-disable-line no-use-before-define
    });

    client.on('packetreceive', () => {
      if (clientRef.current !== client) return;
      markBrokerActivity();
    });

    client.on('offline', () => {
      if (clientRef.current !== client) return;
      handleConnectionLoss(0);
    });

    client.on('close', () => {
      if (clientRef.current !== client) return;
      if (manualDisconnectRef.current) {
        setStatus('disconnected');
        return;
      }
      handleConnectionLoss(RECONNECT_DELAY_MS);
    });

    client.on('error', () => {
      if (clientRef.current !== client) return;
      handleConnectionLoss(0);
    });

    return true;
  }, [destroyClient, flushQueue, handleConnectionLoss, markBrokerActivity, startHealthCheck]);

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

    const fallback = selectFallbackBroker(ct, primaryEdgeIdRef.current, primaryIpRef.current);
    if (!fallback.found) {
      schedulePrimaryRetry();
      return false;
    }

    let fallbackUrl = buildBrokerUrl(fallback.ip);
    if (fallbackUrl === primaryBrokerUrlRef.current && cachedCoreUrlRef.current && cachedCoreUrlRef.current !== fallbackUrl) {
      fallbackUrl = cachedCoreUrlRef.current;
    }

    if (!fallbackUrl || fallbackUrl === activeBrokerUrlRef.current) {
      schedulePrimaryRetry();
      return false;
    }

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
    lastBrokerActivityAtRef.current = 0;
    setCtReceived(false);
    setOnFallback(false);

    return connectToBroker(primaryUrl);
  }, [connectToBroker, destroyClient]);

  const disconnect = useCallback(() => {
    manualDisconnectRef.current = true;
    lastPublishAttemptRef.current = null;
    primaryBrokerUrlRef.current = '';
    primaryIpRef.current = '';
    cachedCoreUrlRef.current = '';
    onFallbackRef.current = false;
    ctRef.current = null;
    queueRef.current = [];
    setQueueSizeFromRef();
    clearPendingPublishes('discard');
    activeBrokerUrlRef.current = '';
    setActiveBrokerUrl('');
    setCtReceived(false);
    setOnFallback(false);
    destroyClient(true);
    setStatus('disconnected');
  }, [clearPendingPublishes, destroyClient, setQueueSizeFromRef]);

  const publish = useCallback(({ publisherId, type, buildingId, cameraId, description = '' }) => {
    const now = Date.now();
    const publishAttemptKey = buildPublishAttemptKey({ type, buildingId, cameraId, description });
    if (shouldSuppressDuplicatePublish(lastPublishAttemptRef.current, publishAttemptKey, now)) {
      return false;
    }
    lastPublishAttemptRef.current = { key: publishAttemptKey, at: now };

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
      sentAt: now,
    };
    const queueEntry = { topic, payload, sentAt: baseLog.sentAt, log: baseLog };

    const client = clientRef.current;
    const brokerLooksStale = isBrokerActivityStale(lastBrokerActivityAtRef.current, now);
    if (!client?.connected || brokerLooksStale) {
      queuePublish(queueEntry);
      upsertEvent({ ...baseLog, status: 'queued' });

      if (!manualDisconnectRef.current && status !== 'connecting') {
        handleConnectionLoss(0);
      }
      return false;
    }

    const pendingEntry = {
      queueEntry,
      queued: false,
      settled: false,
      timeoutId: null,
    };
    pendingEntry.timeoutId = setTimeout(() => {
      const currentPending = pendingPublishesRef.current.get(msg.msg_id);
      if (!currentPending || currentPending.settled) return;

      currentPending.queued = queuePublish(currentPending.queueEntry) || currentPending.queued;
      upsertEvent({
        ...currentPending.queueEntry.log,
        status: 'queued',
        brokerUrl: activeBrokerUrlRef.current || currentPending.queueEntry.log.brokerUrl,
        sentAt: currentPending.queueEntry.sentAt,
      });
      pendingPublishesRef.current.delete(msg.msg_id);

      if (!manualDisconnectRef.current && status !== 'connecting') {
        handleConnectionLoss(0);
      }
    }, PUBLISH_ACK_TIMEOUT_MS);
    pendingPublishesRef.current.set(msg.msg_id, pendingEntry);

    client.publish(topic, payload, { qos: 1 }, (error) => {
      const currentPending = pendingPublishesRef.current.get(msg.msg_id);
      if (!currentPending) return;

      clearTimeout(currentPending.timeoutId);
      currentPending.settled = true;
      pendingPublishesRef.current.delete(msg.msg_id);

      if (error) {
        currentPending.queued = queuePublish(currentPending.queueEntry) || currentPending.queued;
        upsertEvent({
          ...currentPending.queueEntry.log,
          status: 'queued',
          brokerUrl: activeBrokerUrlRef.current || currentPending.queueEntry.log.brokerUrl,
          sentAt: currentPending.queueEntry.sentAt,
        });
        if (!manualDisconnectRef.current && status !== 'connecting') {
          handleConnectionLoss(0);
        }
        return;
      }

      markBrokerActivity();
      upsertEvent({ ...baseLog, status: 'sent', brokerUrl: activeBrokerUrlRef.current, sentAt: Date.now() });
    });

    return true;
  }, [applyOriginRoute, handleConnectionLoss, isDeliveringViaFallback, markBrokerActivity, queuePublish, status, upsertEvent]);

  useEffect(() => () => {
    manualDisconnectRef.current = true;
    clearPendingPublishes('discard');
    destroyClient(true);
  }, [clearPendingPublishes, destroyClient]);

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
