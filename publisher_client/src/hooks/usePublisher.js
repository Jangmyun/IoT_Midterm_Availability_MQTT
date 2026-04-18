// usePublisher.js
// MQTT 연결 + 이벤트 발행 훅
//
// 연결 대상: Edge 노드의 MQTT WebSocket 포트 (ws://<edge-ip>:9001)
// 발행 토픽: campus/data/<type>/<building>/<camera>

import { useRef, useState, useCallback } from 'react';
import mqtt from 'mqtt';
import { buildMessage, buildTopic } from '../mqtt/message.js';

const MAX_LOG = 30;
const CLIENT_ID = `pub-${crypto.randomUUID().slice(0, 8)}`;

export function usePublisher() {
  const [status, setStatus]   = useState('disconnected'); // connecting | connected | disconnected | error
  const [events, setEvents]   = useState([]);             // 발행 로그 (최근 MAX_LOG 건)
  const clientRef             = useRef(null);

  const connect = useCallback((brokerUrl) => {
    // 기존 연결 정리
    if (clientRef.current) {
      clientRef.current.end(true);
      clientRef.current = null;
    }

    setStatus('connecting');

    const client = mqtt.connect(brokerUrl, {
      clientId: CLIENT_ID,
      reconnectPeriod: 3000,
      connectTimeout: 5000,
    });

    clientRef.current = client;

    client.on('connect', () => setStatus('connected'));
    client.on('close',   () => setStatus('disconnected'));
    client.on('error',   () => setStatus('error'));
  }, []);

  const disconnect = useCallback(() => {
    clientRef.current?.end(true);
    clientRef.current = null;
    setStatus('disconnected');
  }, []);

  // publish — 성공하면 true, 미연결이면 false
  const publish = useCallback(({ publisherId, type, buildingId, cameraId, description = '' }) => {
    const client = clientRef.current;
    if (!client?.connected) return false;

    let msg, topic;
    try {
      msg   = buildMessage({ publisherId, type, buildingId, cameraId, description, qos: 1 });
      topic = buildTopic(type, buildingId, cameraId);
    } catch {
      return false;
    }

    if (!topic) return false;

    client.publish(topic, JSON.stringify(msg), { qos: 1 });

    setEvents(prev => [
      { ...msg, topic, sentAt: Date.now() },
      ...prev,
    ].slice(0, MAX_LOG));

    return true;
  }, []);

  return { status, events, connect, disconnect, publish };
}
