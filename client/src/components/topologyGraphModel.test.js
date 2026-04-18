import test from 'node:test';
import assert from 'node:assert/strict';
import { classifyTopologyLink } from './topologyGraphModel.js';

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
