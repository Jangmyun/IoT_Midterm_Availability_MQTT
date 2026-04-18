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
    publisherId: PUBLISHER_ID,
    description: 'Camera auto-detect',
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
