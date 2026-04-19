import test from 'node:test';
import assert from 'node:assert/strict';
import {
  buildAutoNodeAliases,
  buildNodePresentationMap,
  getNodeAliasKey,
  resolveEventNodeId,
  resolveNodeAlias,
} from './nodePresentation.js';

test('buildNodePresentationMap assigns edge numbers ordered by endpoint', () => {
  const topology = {
    nodes: [
      { id: 'edge-b', role: 'NODE', ip: '192.168.0.16', port: 1883 },
      { id: 'edge-a', role: 'NODE', ip: '192.168.0.9', port: 1883 },
    ],
  };

  const displayMap = buildNodePresentationMap(topology, {});

  assert.equal(displayMap.get('edge-a').edgeLabel, 'EDGE 1');
  assert.equal(displayMap.get('edge-b').edgeLabel, 'EDGE 2');
});

test('buildNodePresentationMap includes aliases in graph and list labels', () => {
  const topology = {
    nodes: [
      { id: 'edge-a', role: 'NODE', ip: '192.168.0.9', port: 1883 },
    ],
  };

  const displayMap = buildNodePresentationMap(topology, { 'edge-a': 'Engineering Hall' });

  assert.equal(displayMap.get('edge-a').graphLabel, 'EDGE 1\nEngineering Hall');
  assert.equal(displayMap.get('edge-a').listLabel, 'EDGE 1 · Engineering Hall');
});

test('buildNodePresentationMap uses simplified labels for active and backup cores', () => {
  const topology = {
    active_core_id: 'core-a',
    backup_core_id: 'core-b',
    nodes: [
      { id: 'core-a', role: 'CORE', ip: '192.168.0.7', port: 1883 },
      { id: 'core-b', role: 'CORE', ip: '192.168.0.8', port: 1883 },
    ],
  };

  const displayMap = buildNodePresentationMap(topology, {});

  assert.equal(displayMap.get('core-a').graphLabel, 'ACTIVE\nCORE');
  assert.equal(displayMap.get('core-b').listLabel, 'BACKUP CORE');
});

test('getNodeAliasKey uses endpoint so aliases survive reconnects with new ids', () => {
  assert.equal(
    getNodeAliasKey({ id: 'edge-a', ip: '192.168.0.9', port: 1883 }),
    '192.168.0.9:1883',
  );
});

test('resolveNodeAlias prefers endpoint alias and falls back to legacy id alias', () => {
  assert.equal(
    resolveNodeAlias(
      { id: 'edge-a', ip: '192.168.0.9', port: 1883 },
      { '192.168.0.9:1883': 'Engineering Hall' },
    ),
    'Engineering Hall',
  );

  assert.equal(
    resolveNodeAlias(
      { id: 'edge-a', ip: '192.168.0.9', port: 1883 },
      { 'edge-a': 'Legacy Name' },
    ),
    'Legacy Name',
  );
});

test('buildAutoNodeAliases uses building_id from latest event for each edge endpoint', () => {
  const topology = {
    nodes: [
      { id: 'edge-a', role: 'NODE', ip: '192.168.0.9', port: 1883 },
      { id: 'edge-b', role: 'NODE', ip: '192.168.0.16', port: 1883 },
    ],
  };

  const aliases = buildAutoNodeAliases(topology, [
    {
      source: { id: 'edge-a' },
      route: { original_node: 'edge-a' },
      payload: { building_id: 'Newton 4F', camera_id: 'cam-01' },
    },
    {
      source: { id: 'edge-b' },
      route: { original_node: 'edge-b' },
      payload: { building_id: 'Newton 3F', camera_id: 'cam-02' },
    },
  ]);

  assert.equal(aliases['192.168.0.9:1883'], 'Newton 4F');
  assert.equal(aliases['192.168.0.16:1883'], 'Newton 3F');
});

test('buildAutoNodeAliases prefers route.original_node so failover does not relabel relay edge', () => {
  const topology = {
    nodes: [
      { id: 'edge-a', role: 'NODE', ip: '192.168.0.9', port: 1883 },
      { id: 'edge-b', role: 'NODE', ip: '192.168.0.16', port: 1883 },
    ],
  };

  const aliases = buildAutoNodeAliases(topology, [
    {
      source: { id: 'edge-b' },
      route: { original_node: 'edge-a' },
      payload: { building_id: 'Newton 4F', camera_id: 'cam-01' },
    },
  ]);

  assert.equal(aliases['192.168.0.9:1883'], 'Newton 4F');
  assert.equal(aliases['192.168.0.16:1883'], undefined);
});

test('resolveEventNodeId prefers route.original_node and falls back to source.id', () => {
  assert.equal(
    resolveEventNodeId({
      source: { id: 'relay-edge' },
      route: { original_node: 'origin-edge' },
    }),
    'origin-edge',
  );

  assert.equal(
    resolveEventNodeId({
      source: { id: 'relay-edge' },
      route: { original_node: '' },
    }),
    'relay-edge',
  );
});
