import test from 'node:test';
import assert from 'node:assert/strict';
import { buildEventPresentationSnapshots } from './eventSnapshot.js';

const ORIGIN_EDGE_ID = 'edge-origin';
const RELAY_EDGE_ID = 'edge-relay';
const EVENT_ID = '00000000-0000-4000-8000-000000000001';

function makeEvent() {
  return {
    msg_id: EVENT_ID,
    timestamp: '2026-04-19T10:00:00Z',
    source: { id: RELAY_EDGE_ID },
    route: { original_node: ORIGIN_EDGE_ID },
    payload: {
      building_id: 'Newton 4F',
      camera_id: 'cam-01',
      description: JSON.stringify({
        source: { id: 'publisher-1' },
        payload: { description: 'manual' },
        via_failover: true,
        intended_edge_ip: '192.168.0.18',
      }),
    },
  };
}

test('buildEventPresentationSnapshots preserves prior event views when topology changes later', () => {
  const event = makeEvent();
  const initialNodeById = new Map([
    [ORIGIN_EDGE_ID, { id: ORIGIN_EDGE_ID, ip: '192.168.0.18', port: 1883 }],
    [RELAY_EDGE_ID, { id: RELAY_EDGE_ID, ip: '192.168.0.16', port: 1883 }],
  ]);
  const initialDisplayMap = new Map([
    [ORIGIN_EDGE_ID, { edgeLabel: 'EDGE 3', listLabel: 'EDGE 3', alias: 'Newton 4F' }],
    [RELAY_EDGE_ID, { edgeLabel: 'EDGE 2', listLabel: 'EDGE 2', alias: 'Newton 3F' }],
  ]);

  const firstSnapshots = buildEventPresentationSnapshots(
    [event],
    new Map(),
    initialNodeById,
    initialDisplayMap,
  );

  const changedNodeById = new Map([
    [RELAY_EDGE_ID, { id: RELAY_EDGE_ID, ip: '192.168.0.16', port: 1883 }],
  ]);
  const changedDisplayMap = new Map([
    [RELAY_EDGE_ID, { edgeLabel: 'EDGE 1', listLabel: 'EDGE 1', alias: 'Newton 3F' }],
  ]);

  const secondSnapshots = buildEventPresentationSnapshots(
    [event],
    firstSnapshots,
    changedNodeById,
    changedDisplayMap,
  );

  assert.equal(secondSnapshots.get(EVENT_ID), firstSnapshots.get(EVENT_ID));
  assert.equal(secondSnapshots.get(EVENT_ID).viaFailover, true);
  assert.equal(secondSnapshots.get(EVENT_ID).edgeLabel, 'EDGE 3');
  assert.equal(secondSnapshots.get(EVENT_ID).transitEdgeLabel, 'EDGE 2');
});

test('buildEventPresentationSnapshots computes a new view when an event is first seen', () => {
  const event = makeEvent();
  const snapshots = buildEventPresentationSnapshots(
    [event],
    new Map(),
    new Map([
      [ORIGIN_EDGE_ID, { id: ORIGIN_EDGE_ID, ip: '192.168.0.18', port: 1883 }],
      [RELAY_EDGE_ID, { id: RELAY_EDGE_ID, ip: '192.168.0.16', port: 1883 }],
    ]),
    new Map([
      [ORIGIN_EDGE_ID, { edgeLabel: 'EDGE 3', listLabel: 'EDGE 3', alias: 'Newton 4F' }],
      [RELAY_EDGE_ID, { edgeLabel: 'EDGE 2', listLabel: 'EDGE 2', alias: 'Newton 3F' }],
    ]),
  );

  assert.equal(snapshots.get(EVENT_ID).sourceTitle, 'Newton 4F');
  assert.equal(snapshots.get(EVENT_ID).viaFailover, true);
});
