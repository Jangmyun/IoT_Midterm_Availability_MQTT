import test from 'node:test';
import assert from 'node:assert/strict';
import {
  formatClockTime,
  formatDelayLabel,
  formatEventSourceOption,
  formatEventTime,
  getEventPresentation,
  parseNestedPublisherMessage,
} from './eventPresentation.js';

const EDGE_ID = '81fac066-1ebe-4a7f-8e75-5e090711cc47';
const PUBLISHER_ID = '0155558d-1234-4abc-8def-1234567890ab';

test('parseNestedPublisherMessage extracts publisher metadata from nested JSON payload', () => {
  const raw = JSON.stringify({
    source: { id: PUBLISHER_ID },
    payload: { description: 'auto-detect' },
  });

  assert.deepEqual(parseNestedPublisherMessage(raw), {
    publisherId:    PUBLISHER_ID,
    description:    'Camera auto-detect',
    viaFailover:    false,
    intendedEdgeIp: '',
    wasQueued:      false,
    createdAt:      '',
  });
});

test('getEventPresentation resolves source edge and readable location details', () => {
  const nodeById = new Map([
    [EDGE_ID, { id: EDGE_ID, ip: '192.168.0.9', port: 1883 }],
  ]);
  const nodeDisplayMap = new Map([
    [EDGE_ID, {
      edgeLabel: 'EDGE 1',
      listLabel: 'EDGE 1 · Engineering Hall',
      alias: 'Engineering Hall',
    }],
  ]);

  const event = {
    timestamp: '2026-04-18T14:04:52Z',
    source: { id: EDGE_ID },
    payload: {
      building_id: 'building-a',
      camera_id: 'cam-01',
      description: JSON.stringify({
        source: { id: PUBLISHER_ID },
        payload: { description: 'manual' },
      }),
    },
  };

  assert.deepEqual(getEventPresentation(event, nodeById, nodeDisplayMap), {
    sourceId: EDGE_ID,
    sourceIp: '192.168.0.9',
    sourcePort: '1883',
    sourceEndpoint: '192.168.0.9:1883',
    edgeLabel: 'EDGE 1',
    sourceTitle: 'Engineering Hall',
    sourceAlias: 'Engineering Hall',
    locationLabel: 'building-a / cam-01',
    descriptionLabel: 'Manual trigger',
    publisherId: PUBLISHER_ID,
    viaFailover:      false,
    intendedEdgeLabel: '',
    wasQueued:        false,
    queueDelayMs:     0,
  });
});

test('formatEventSourceOption falls back to short id when topology details are missing', () => {
  assert.equal(formatEventSourceOption(EDGE_ID, null, null), '81fac066…');
});

test('formatEventTime returns hh:mm:ss extracted from ISO timestamps', () => {
  assert.equal(formatEventTime('2026-04-18T14:04:52Z'), '14:04:52');
});

test('formatClockTime formats client-side timestamps', () => {
  assert.equal(formatClockTime(new Date(2026, 3, 18, 14, 4, 52).getTime()), '14:04:52');
});

test('formatDelayLabel renders readable delayed-delivery text', () => {
  assert.equal(formatDelayLabel(2500), '3s late');
  assert.equal(formatDelayLabel(65000), '1m 5s late');
});

const INTENDED_EDGE_ID = '77777777-aaaa-4bbb-8ccc-000000000001';
const INTENDED_IP = '192.168.0.8';

test('getEventPresentation resolves intendedEdgeLabel from via_failover metadata', () => {
  const nodeById = new Map([
    [EDGE_ID,         { id: EDGE_ID,         ip: '192.168.0.9', port: 1883 }],
    [INTENDED_EDGE_ID,{ id: INTENDED_EDGE_ID, ip: INTENDED_IP,  port: 1883 }],
  ]);
  const nodeDisplayMap = new Map([
    [EDGE_ID,         { edgeLabel: 'EDGE 2', listLabel: 'EDGE 2', alias: '' }],
    [INTENDED_EDGE_ID,{ edgeLabel: 'EDGE 1', listLabel: 'EDGE 1', alias: '' }],
  ]);

  const event = {
    timestamp: '2026-04-18T14:04:52Z',
    source: { id: EDGE_ID },
    payload: {
      building_id: 'b',
      camera_id:   'c',
      description: JSON.stringify({
        source: { id: PUBLISHER_ID },
        payload: { description: '' },
        via_failover:     true,
        intended_edge_ip: INTENDED_IP,
      }),
    },
  };

  const result = getEventPresentation(event, nodeById, nodeDisplayMap);
  assert.equal(result.viaFailover, true);
  assert.equal(result.intendedEdgeLabel, 'EDGE 1');
});

test('getEventPresentation calculates queueDelayMs when was_queued is true', () => {
  const nodeById = new Map([
    [EDGE_ID, { id: EDGE_ID, ip: '192.168.0.9', port: 1883 }],
  ]);
  const nodeDisplayMap = new Map([
    [EDGE_ID, { edgeLabel: 'EDGE 1', listLabel: 'EDGE 1', alias: '' }],
  ]);

  const event = {
    timestamp: '2026-04-18T14:05:02Z',
    source: { id: EDGE_ID },
    payload: {
      building_id: 'b',
      camera_id:   'c',
      description: JSON.stringify({
        source: { id: PUBLISHER_ID },
        payload: { description: '' },
        was_queued:  true,
        created_at:  '2026-04-18T14:04:32Z',
      }),
    },
  };

  const result = getEventPresentation(event, nodeById, nodeDisplayMap);
  assert.equal(result.wasQueued, true);
  assert.equal(result.queueDelayMs, 30000);
});
