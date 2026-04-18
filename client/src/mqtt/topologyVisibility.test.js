import test from 'node:test';
import assert from 'node:assert/strict';
import { buildPresentationTopology } from './topologyVisibility.js';

const ACTIVE_CORE = 'aaaaaaaa-1111-2222-3333-444444444444';
const BACKUP_CORE = 'bbbbbbbb-1111-2222-3333-555555555555';
const OLD_CORE = 'cccccccc-1111-2222-3333-666666666666';
const EDGE_A = 'dddddddd-1111-2222-3333-777777777777';
const EDGE_B = 'eeeeeeee-1111-2222-3333-888888888888';

function makeTopology() {
  return {
    version: 10,
    last_update: '2026-04-17T05:00:00Z',
    active_core_id: ACTIVE_CORE,
    backup_core_id: '',
    nodes: [
      { id: ACTIVE_CORE, role: 'CORE', ip: '192.168.0.7', port: 1883, status: 'ONLINE', hop_to_core: 0 },
      { id: OLD_CORE, role: 'CORE', ip: '192.168.0.16', port: 1883, status: 'ONLINE', hop_to_core: 1 },
      { id: EDGE_A, role: 'NODE', ip: '192.168.0.18', port: 1883, status: 'ONLINE', hop_to_core: 1 },
      { id: EDGE_B, role: 'NODE', ip: '192.168.0.19', port: 1883, status: 'OFFLINE', hop_to_core: 1 },
    ],
    links: [
      { from_id: ACTIVE_CORE, to_id: EDGE_A, rtt_ms: 1.2 },
      { from_id: OLD_CORE, to_id: EDGE_A, rtt_ms: 1.3 },
      { from_id: ACTIVE_CORE, to_id: EDGE_B, rtt_ms: 1.4 },
    ],
  };
}

test('buildPresentationTopology keeps only the current active core after failover', () => {
  const topology = makeTopology();

  const visible = buildPresentationTopology(topology);

  assert.deepEqual(
    visible.nodes.map((node) => node.id),
    [ACTIVE_CORE, EDGE_A],
  );
});

test('buildPresentationTopology keeps backup core when it is explicitly assigned', () => {
  const topology = {
    ...makeTopology(),
    backup_core_id: BACKUP_CORE,
    nodes: [
      ...makeTopology().nodes,
      { id: BACKUP_CORE, role: 'CORE', ip: '192.168.0.20', port: 1883, status: 'ONLINE', hop_to_core: 1 },
    ],
    links: [
      ...makeTopology().links,
      { from_id: ACTIVE_CORE, to_id: BACKUP_CORE, rtt_ms: 0.4 },
    ],
  };

  const visible = buildPresentationTopology(topology);

  assert.deepEqual(
    visible.nodes.map((node) => node.id),
    [ACTIVE_CORE, EDGE_A, BACKUP_CORE],
  );
});

test('buildPresentationTopology hides nodes suppressed by disconnect alerts', () => {
  const topology = {
    ...makeTopology(),
    nodes: [
      { id: ACTIVE_CORE, role: 'CORE', ip: '192.168.0.7', port: 1883, status: 'ONLINE', hop_to_core: 0 },
      { id: EDGE_A, role: 'NODE', ip: '192.168.0.18', port: 1883, status: 'ONLINE', hop_to_core: 1 },
    ],
    links: [
      { from_id: ACTIVE_CORE, to_id: EDGE_A, rtt_ms: 1.2 },
    ],
  };

  const visible = buildPresentationTopology(topology, new Set([EDGE_A]));

  assert.deepEqual(
    visible.nodes.map((node) => node.id),
    [ACTIVE_CORE],
  );
  assert.equal(visible.links.length, 0);
});
