import test from 'node:test';
import assert from 'node:assert/strict';
import {
  buildBrokerUrl,
  resolveBackupReconnectTarget,
  selectPromotedActiveNode,
  sameBrokerHost,
} from './failover.js';

const ACTIVE_CORE = 'aaaaaaaa-1111-2222-3333-444444444444';
const BACKUP_CORE = 'bbbbbbbb-1111-2222-3333-555555555555';
const EDGE_NODE = 'cccccccc-1111-2222-3333-666666666666';

function makeTopology({ activeCoreId = ACTIVE_CORE, backupCoreId = BACKUP_CORE } = {}) {
  return {
    version: 1,
    last_update: '2026-04-17T05:00:00Z',
    active_core_id: activeCoreId,
    backup_core_id: backupCoreId,
    nodes: [
      {
        id: ACTIVE_CORE,
        role: 'CORE',
        ip: '192.168.0.7',
        port: 1883,
        status: 'ONLINE',
        hop_to_core: 0,
      },
      {
        id: BACKUP_CORE,
        role: 'CORE',
        ip: '192.168.0.16',
        port: 1883,
        status: 'ONLINE',
        hop_to_core: 1,
      },
      {
        id: EDGE_NODE,
        role: 'NODE',
        ip: '192.168.0.18',
        port: 1883,
        status: 'ONLINE',
        hop_to_core: 1,
      },
    ],
    links: [],
  };
}

test('buildBrokerUrl keeps websocket port while swapping host', () => {
  assert.equal(
    buildBrokerUrl('ws://192.168.0.7:9001', '192.168.0.16'),
    'ws://192.168.0.16:9001',
  );
});

test('sameBrokerHost treats localhost and 127.0.0.1 as equivalent', () => {
  assert.equal(sameBrokerHost('localhost', '127.0.0.1'), true);
  assert.equal(sameBrokerHost('192.168.0.7', '127.0.0.1'), false);
});

test('resolveBackupReconnectTarget uses topology backup node first', () => {
  const topology = makeTopology();
  const fallbackNode = {
    id: 'dddddddd-1111-2222-3333-777777777777',
    ip: '10.0.0.5',
    status: 'ONLINE',
  };

  const selected = resolveBackupReconnectTarget(topology, fallbackNode);
  assert.equal(selected?.id, BACKUP_CORE);
});

test('resolveBackupReconnectTarget falls back when backup_core_id is already cleared', () => {
  const topology = makeTopology({ activeCoreId: BACKUP_CORE, backupCoreId: '' });
  const fallbackNode = topology.nodes.find(node => node.id === BACKUP_CORE);

  const selected = resolveBackupReconnectTarget(topology, fallbackNode);
  assert.equal(selected?.id, BACKUP_CORE);
});

test('selectPromotedActiveNode picks the new active broker when active_core_id changes', () => {
  const previousTopology = makeTopology();
  const nextTopology = makeTopology({ activeCoreId: BACKUP_CORE, backupCoreId: '' });

  const selected = selectPromotedActiveNode(
    previousTopology,
    nextTopology,
    'ws://192.168.0.7:9001',
  );

  assert.equal(selected?.id, BACKUP_CORE);
  assert.equal(selected?.ip, '192.168.0.16');
});

test('selectPromotedActiveNode ignores CT updates when already connected to the promoted broker', () => {
  const previousTopology = makeTopology();
  const nextTopology = makeTopology({ activeCoreId: BACKUP_CORE, backupCoreId: '' });

  const selected = selectPromotedActiveNode(
    previousTopology,
    nextTopology,
    'ws://192.168.0.16:9001',
  );

  assert.equal(selected, null);
});
