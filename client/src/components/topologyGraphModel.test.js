import test from 'node:test';
import assert from 'node:assert/strict';
import {
  buildTopologyNodeLabel,
  buildTopologyNodePositions,
  classifyTopologyLink,
  classifyTopologyNode,
} from './topologyGraphModel.js';

const ACTIVE_CORE = 'active-core-uuid';
const BACKUP_CORE = 'backup-core-uuid';
const EDGE_A = 'edge-a-uuid';
const EDGE_B = 'edge-b-uuid';

function makeTopology() {
  return {
    active_core_id: ACTIVE_CORE,
    backup_core_id: BACKUP_CORE,
    nodes: [
      { id: ACTIVE_CORE, role: 'CORE', status: 'ONLINE' },
      { id: BACKUP_CORE, role: 'CORE', status: 'ONLINE' },
      { id: EDGE_A, role: 'NODE', status: 'ONLINE' },
      { id: EDGE_B, role: 'NODE', status: 'ONLINE' },
    ],
    links: [],
  };
}

test('classifyTopologyLink marks active core paths as active-link', () => {
  const topology = makeTopology();

  assert.equal(
    classifyTopologyLink(topology, { from_id: ACTIVE_CORE, to_id: EDGE_A }),
    'active-link',
  );
  assert.equal(
    classifyTopologyLink(topology, { from_id: EDGE_A, to_id: ACTIVE_CORE }),
    'active-link',
  );
});

test('classifyTopologyLink marks backup core paths as backup-link', () => {
  const topology = makeTopology();

  assert.equal(
    classifyTopologyLink(topology, { from_id: BACKUP_CORE, to_id: EDGE_B }),
    'backup-link',
  );
  assert.equal(
    classifyTopologyLink(topology, { from_id: EDGE_B, to_id: BACKUP_CORE }),
    'backup-link',
  );
});

test('classifyTopologyLink leaves non core-node links unchanged', () => {
  const topology = makeTopology();

  assert.equal(
    classifyTopologyLink(topology, { from_id: EDGE_A, to_id: EDGE_B }),
    'default-link',
  );
  assert.equal(
    classifyTopologyLink(topology, { from_id: ACTIVE_CORE, to_id: BACKUP_CORE }),
    'default-link',
  );
});

test('classifyTopologyNode distinguishes active and backup cores', () => {
  const topology = makeTopology();

  assert.equal(
    classifyTopologyNode(topology, { id: ACTIVE_CORE, role: 'CORE', status: 'ONLINE' }),
    'active-core',
  );
  assert.equal(
    classifyTopologyNode(topology, { id: BACKUP_CORE, role: 'CORE', status: 'ONLINE' }),
    'backup-core',
  );
  assert.equal(
    classifyTopologyNode(topology, { id: EDGE_A, role: 'NODE', status: 'OFFLINE' }),
    'offline-node',
  );
});

test('buildTopologyNodeLabel prefixes active and backup core labels', () => {
  const topology = makeTopology();

  assert.equal(
    buildTopologyNodeLabel(topology, { id: ACTIVE_CORE, role: 'CORE' }).startsWith('CORE\n'),
    true,
  );
  assert.equal(
    buildTopologyNodeLabel(topology, { id: BACKUP_CORE, role: 'CORE' }).startsWith('BACKUP\n'),
    true,
  );
  assert.equal(
    buildTopologyNodeLabel(topology, { id: EDGE_A, role: 'NODE' }),
    EDGE_A.slice(0, 8),
  );
});

test('buildTopologyNodePositions keeps active and backup cores together on the top row', () => {
  const topology = makeTopology();
  const positions = buildTopologyNodePositions(topology, topology.nodes);

  assert.equal(positions[ACTIVE_CORE].y, positions[BACKUP_CORE].y);
  assert.ok(positions[ACTIVE_CORE].x < positions[BACKUP_CORE].x);
  assert.ok(positions[EDGE_A].y > positions[ACTIVE_CORE].y);
  assert.ok(positions[EDGE_B].y > positions[BACKUP_CORE].y);

  const distinctColumns = new Set(
    Object.values(positions).map((position) => Math.round(position.x / 10)),
  );
  assert.ok(distinctColumns.size >= 3);
});
