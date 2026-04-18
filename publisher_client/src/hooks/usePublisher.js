import { useCallback, useEffect, useRef, useState } from 'react';
import mqtt from 'mqtt';
import { buildMessage, buildTopic } from '../mqtt/message.js';
import { parseBrokerCandidates, getNextBrokerIndex } from '../mqtt/failover.js';
import { generateUuid } from '../utils/uuid.js';

const MAX_LOG = 30;
const MAX_QUEUE = 100;
const CLIENT_ID = `pub-${generateUuid().slice(0, 8)}`;
const RECONNECT_DELAY_MS = 1500;

function trimEventLog(events) {
  return events.slice(0, MAX_LOG);
}

export function usePublisher() {
  const [status, setStatus] = useState('disconnected');
  const [events, setEvents] = useState([]);
  const [activeBrokerUrl, setActiveBrokerUrl] = useState('');
  const [queueSize, setQueueSize] = useState(0);

  const clientRef = useRef(null);
  const brokersRef = useRef([]);
  const currentBrokerIndexRef = useRef(-1);
  const reconnectTimerRef = useRef(null);
  const manualDisconnectRef = useRef(false);
  const queueRef = useRef([]);
  const activeBrokerUrlRef = useRef('');

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

  const flushQueue = useCallback(() => {
    const client = clientRef.current;
    if (!client?.connected || queueRef.current.length === 0) return;

    const queuedItems = [...queueRef.current];
    queueRef.current = [];
    setQueueSizeFromRef();

    queuedItems.forEach((item) => {
      client.publish(item.topic, item.payload, { qos: 1 }, (error) => {
        if (error) {
          queueRef.current.unshift(item);
          setQueueSizeFromRef();
          upsertEvent({
            ...item.log,
            status: 'queued',
            brokerUrl: activeBrokerUrlRef.current,
            sentAt: item.sentAt,
          });
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
  }, [setQueueSizeFromRef, upsertEvent]);

  const connectToIndex = useCallback((index) => {
    const brokerUrl = brokersRef.current[index];
    if (!brokerUrl) {
      setStatus('error');
      return false;
    }

    destroyClient(true);
    currentBrokerIndexRef.current = index;
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
      flushQueue();
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
        const nextIndex = getNextBrokerIndex(currentBrokerIndexRef.current, brokersRef.current.length);
        connectToIndex(nextIndex);
      }, RECONNECT_DELAY_MS);
    });

    client.on('error', () => {
      if (clientRef.current !== client) return;
      setStatus('error');
      client.end(true);
    });

    return true;
  }, [clearReconnectTimer, destroyClient, flushQueue]);

  const connect = useCallback((rawBrokerInput) => {
    const brokers = parseBrokerCandidates(rawBrokerInput);
    manualDisconnectRef.current = false;

    if (brokers.length === 0) {
      brokersRef.current = [];
      currentBrokerIndexRef.current = -1;
      activeBrokerUrlRef.current = '';
      setActiveBrokerUrl('');
      destroyClient(true);
      setStatus('error');
      return false;
    }

    brokersRef.current = brokers;
    return connectToIndex(0);
  }, [connectToIndex, destroyClient]);

  const disconnect = useCallback(() => {
    manualDisconnectRef.current = true;
    brokersRef.current = [];
    currentBrokerIndexRef.current = -1;
    activeBrokerUrlRef.current = '';
    setActiveBrokerUrl('');
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
      topic = buildTopic(type, buildingId, cameraId);
    } catch {
      return false;
    }

    if (!topic) return false;

    const payload = JSON.stringify(msg);
    const baseLog = {
      ...msg,
      topic,
      brokerUrl: activeBrokerUrlRef.current,
      sentAt: Date.now(),
    };

    const queueEntry = {
      topic,
      payload,
      sentAt: baseLog.sentAt,
      log: baseLog,
    };

    const client = clientRef.current;
    if (!client?.connected) {
      queuePublish(queueEntry);
      upsertEvent({ ...baseLog, status: 'queued' });

      if (!manualDisconnectRef.current && brokersRef.current.length > 0 && !clientRef.current) {
        const nextIndex = currentBrokerIndexRef.current >= 0 ? currentBrokerIndexRef.current : 0;
        connectToIndex(nextIndex);
      }
      return false;
    }

    client.publish(topic, payload, { qos: 1 }, (error) => {
      if (error) {
        queuePublish(queueEntry);
        upsertEvent({ ...baseLog, status: 'queued' });
        return;
      }

      upsertEvent({
        ...baseLog,
        status: 'sent',
        brokerUrl: activeBrokerUrlRef.current,
        sentAt: Date.now(),
      });
    });

    return true;
  }, [connectToIndex, queuePublish, upsertEvent]);

  useEffect(() => () => {
    manualDisconnectRef.current = true;
    destroyClient(true);
  }, [destroyClient]);

  return {
    status,
    events,
    activeBrokerUrl,
    queueSize,
    connect,
    disconnect,
    publish,
  };
}
