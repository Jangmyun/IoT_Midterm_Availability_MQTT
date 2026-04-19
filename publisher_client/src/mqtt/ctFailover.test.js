import test from 'node:test';
import assert from 'node:assert/strict';
import {
  findPreferredCoreBroker,
  selectFallbackBroker,
  shouldAcceptCtUpdate,
  shouldReturnToPrimary,
  resolvePrimaryEdgeId,
} from './ctFailover.js';

const PRIMARY_ID = 'primary-edge-uuid';

function makeNode(id, role, status, ip, port, hop) {
  return { id, role, status, ip, port: port ?? 1883, hop_to_core: hop ?? 1 };
}

function makeCt(nodes, links = []) {
  return { nodes, links };
}

// ── selectFallbackBroker ────────────────────────────────────────────

test('selectFallbackBroker: CT null → found: false', () => {
  assert.deepEqual(selectFallbackBroker(null, PRIMARY_ID), { found: false });
});

test('selectFallbackBroker: CT without nodes → found: false', () => {
  assert.deepEqual(selectFallbackBroker({}, PRIMARY_ID), { found: false });
});

test('selectFallbackBroker: 모든 후보 OFFLINE → found: false', () => {
  const ct = makeCt([
    makeNode(PRIMARY_ID, 'NODE', 'ONLINE', '192.168.0.9', 1883, 1),
    makeNode('edge-b', 'NODE', 'OFFLINE', '192.168.0.16', 1883, 1),
  ]);
  assert.deepEqual(selectFallbackBroker(ct, PRIMARY_ID), { found: false });
});

test('selectFallbackBroker: Edge 하나 ONLINE → 해당 Edge 선택', () => {
  const ct = makeCt([
    makeNode(PRIMARY_ID, 'NODE', 'ONLINE', '192.168.0.9', 1883, 1),
    makeNode('edge-b', 'NODE', 'ONLINE', '192.168.0.16', 1883, 1),
  ]);
  const result = selectFallbackBroker(ct, PRIMARY_ID);
  assert.equal(result.found, true);
  assert.equal(result.id, 'edge-b');
  assert.equal(result.ip, '192.168.0.16');
});

test('selectFallbackBroker: Edge vs Core → Edge 우선', () => {
  const ct = makeCt([
    makeNode(PRIMARY_ID, 'NODE', 'ONLINE', '192.168.0.9', 1883, 1),
    makeNode('core-x', 'CORE', 'ONLINE', '192.168.0.7', 1883, 0),
    makeNode('edge-b', 'NODE', 'ONLINE', '192.168.0.16', 1883, 1),
  ]);
  const result = selectFallbackBroker(ct, PRIMARY_ID);
  assert.equal(result.found, true);
  assert.equal(result.id, 'edge-b');
});

test('selectFallbackBroker: core fallback 시 active core 우선', () => {
  const ct = {
    active_core_id: 'core-active',
    backup_core_id: 'core-backup',
    nodes: [
      makeNode(PRIMARY_ID, 'NODE', 'OFFLINE', '192.168.0.9', 1883, 1),
      makeNode('core-active', 'CORE', 'ONLINE', '192.168.0.7', 1883, 0),
      makeNode('core-backup', 'CORE', 'ONLINE', '192.168.0.8', 1883, 1),
    ],
    links: [
      { from_id: PRIMARY_ID, to_id: 'core-active', rtt_ms: 30 },
      { from_id: PRIMARY_ID, to_id: 'core-backup', rtt_ms: 10 },
    ],
  };
  const result = selectFallbackBroker(ct, PRIMARY_ID);
  assert.equal(result.found, true);
  assert.equal(result.id, 'core-active');
});

test('selectFallbackBroker: RTT 낮은 Edge 선택', () => {
  const ct = makeCt(
    [
      makeNode(PRIMARY_ID, 'NODE', 'ONLINE', '192.168.0.9', 1883, 1),
      makeNode('edge-b', 'NODE', 'ONLINE', '192.168.0.16', 1883, 1),
      makeNode('edge-c', 'NODE', 'ONLINE', '192.168.0.20', 1883, 1),
    ],
    [
      { from_id: PRIMARY_ID, to_id: 'edge-b', rtt_ms: 80 },
      { from_id: PRIMARY_ID, to_id: 'edge-c', rtt_ms: 30 },
    ]
  );
  const result = selectFallbackBroker(ct, PRIMARY_ID);
  assert.equal(result.found, true);
  assert.equal(result.id, 'edge-c');
});

test('selectFallbackBroker: reverse link만 있어도 RTT를 사용한다', () => {
  const ct = makeCt(
    [
      makeNode(PRIMARY_ID, 'NODE', 'ONLINE', '192.168.0.9', 1883, 1),
      makeNode('edge-b', 'NODE', 'ONLINE', '192.168.0.16', 1883, 1),
      makeNode('edge-c', 'NODE', 'ONLINE', '192.168.0.20', 1883, 1),
    ],
    [
      { from_id: 'edge-b', to_id: PRIMARY_ID, rtt_ms: 70 },
      { from_id: 'edge-c', to_id: PRIMARY_ID, rtt_ms: 15 },
    ]
  );
  const result = selectFallbackBroker(ct, PRIMARY_ID);
  assert.equal(result.found, true);
  assert.equal(result.id, 'edge-c');
});

test('selectFallbackBroker: RTT 동점 → hop 낮은 Edge 선택', () => {
  const ct = makeCt(
    [
      makeNode(PRIMARY_ID, 'NODE', 'ONLINE', '192.168.0.9', 1883, 1),
      makeNode('edge-b', 'NODE', 'ONLINE', '192.168.0.16', 1883, 2),
      makeNode('edge-c', 'NODE', 'ONLINE', '192.168.0.20', 1883, 1),
    ],
    [
      { from_id: PRIMARY_ID, to_id: 'edge-b', rtt_ms: 50 },
      { from_id: PRIMARY_ID, to_id: 'edge-c', rtt_ms: 50 },
    ]
  );
  const result = selectFallbackBroker(ct, PRIMARY_ID);
  assert.equal(result.found, true);
  assert.equal(result.id, 'edge-c');
});

test('selectFallbackBroker: primaryEdgeId 자신은 제외', () => {
  const ct = makeCt([
    makeNode(PRIMARY_ID, 'NODE', 'ONLINE', '192.168.0.9', 1883, 1),
  ]);
  assert.deepEqual(selectFallbackBroker(ct, PRIMARY_ID), { found: false });
});

test('selectFallbackBroker: primaryEdgeId null이면 자신 제외 없이 전체 탐색', () => {
  const ct = makeCt([
    makeNode('edge-a', 'NODE', 'ONLINE', '192.168.0.9', 1883, 1),
  ]);
  const result = selectFallbackBroker(ct, null);
  assert.equal(result.found, true);
  assert.equal(result.id, 'edge-a');
});

test('selectFallbackBroker: primaryEdgeId 미해결이어도 primary IP는 제외한다', () => {
  const ct = makeCt(
    [
      makeNode('edge-a', 'NODE', 'ONLINE', '192.168.0.9', 1883, 1),
      makeNode('edge-b', 'NODE', 'ONLINE', '192.168.0.16', 1883, 1),
    ],
    [
      { from_id: 'edge-a', to_id: 'edge-b', rtt_ms: 20 },
    ]
  );
  const result = selectFallbackBroker(ct, null, '192.168.0.9');
  assert.equal(result.found, true);
  assert.equal(result.id, 'edge-b');
});

// ── shouldReturnToPrimary ───────────────────────────────────────────

test('shouldReturnToPrimary: primary OFFLINE → false', () => {
  const ct = makeCt([makeNode(PRIMARY_ID, 'NODE', 'OFFLINE', '192.168.0.9', 1883, 1)]);
  assert.equal(shouldReturnToPrimary(ct, PRIMARY_ID), false);
});

test('shouldReturnToPrimary: primary ONLINE → true', () => {
  const ct = makeCt([makeNode(PRIMARY_ID, 'NODE', 'ONLINE', '192.168.0.9', 1883, 1)]);
  assert.equal(shouldReturnToPrimary(ct, PRIMARY_ID), true);
});

test('shouldReturnToPrimary: primaryEdgeId null → false', () => {
  const ct = makeCt([makeNode(PRIMARY_ID, 'NODE', 'ONLINE', '192.168.0.9', 1883, 1)]);
  assert.equal(shouldReturnToPrimary(ct, null), false);
});

// ── findPreferredCoreBroker ─────────────────────────────────────────

test('findPreferredCoreBroker: active core 우선 선택', () => {
  const ct = {
    active_core_id: 'core-active',
    backup_core_id: 'core-backup',
    nodes: [
      makeNode('core-active', 'CORE', 'ONLINE', '192.168.0.7', 1883, 0),
      makeNode('core-backup', 'CORE', 'ONLINE', '192.168.0.8', 1883, 1),
    ],
    links: [],
  };
  assert.deepEqual(findPreferredCoreBroker(ct), {
    id: 'core-active',
    ip: '192.168.0.7',
    port: 1883,
  });
});

test('findPreferredCoreBroker: active core 없으면 backup core 선택', () => {
  const ct = {
    active_core_id: 'core-active',
    backup_core_id: 'core-backup',
    nodes: [
      makeNode('core-active', 'CORE', 'OFFLINE', '192.168.0.7', 1883, 0),
      makeNode('core-backup', 'CORE', 'ONLINE', '192.168.0.8', 1883, 1),
    ],
    links: [],
  };
  assert.deepEqual(findPreferredCoreBroker(ct), {
    id: 'core-backup',
    ip: '192.168.0.8',
    port: 1883,
  });
});

// ── shouldAcceptCtUpdate ────────────────────────────────────────────

test('shouldAcceptCtUpdate: 첫 CT는 수락', () => {
  const nextCt = {
    version: 3,
    active_core_id: 'core-active',
    backup_core_id: '',
    nodes: [],
    links: [],
  };
  assert.equal(shouldAcceptCtUpdate(null, nextCt), true);
});

test('shouldAcceptCtUpdate: stale CT는 거부', () => {
  const previousCt = {
    version: 10,
    active_core_id: 'core-active',
    backup_core_id: '',
    nodes: [],
    links: [],
  };
  const nextCt = {
    version: 9,
    active_core_id: 'core-active',
    backup_core_id: '',
    nodes: [],
    links: [],
  };
  assert.equal(shouldAcceptCtUpdate(previousCt, nextCt), false);
});

test('shouldAcceptCtUpdate: newer CT는 수락', () => {
  const previousCt = {
    version: 9,
    active_core_id: 'core-active',
    backup_core_id: '',
    nodes: [],
    links: [],
  };
  const nextCt = {
    version: 10,
    active_core_id: 'core-active',
    backup_core_id: '',
    nodes: [],
    links: [],
  };
  assert.equal(shouldAcceptCtUpdate(previousCt, nextCt), true);
});

// ── resolvePrimaryEdgeId ────────────────────────────────────────────

test('resolvePrimaryEdgeId: IP 매칭 → UUID 반환', () => {
  const ct = makeCt([
    makeNode('core-x', 'CORE', 'ONLINE', '192.168.0.7', 1883, 0),
    makeNode(PRIMARY_ID, 'NODE', 'ONLINE', '192.168.0.9', 1883, 1),
  ]);
  assert.equal(resolvePrimaryEdgeId(ct, '192.168.0.9'), PRIMARY_ID);
});

test('resolvePrimaryEdgeId: 없는 IP → null', () => {
  const ct = makeCt([makeNode(PRIMARY_ID, 'NODE', 'ONLINE', '192.168.0.9', 1883, 1)]);
  assert.equal(resolvePrimaryEdgeId(ct, '192.168.0.99'), null);
});

test('resolvePrimaryEdgeId: 포트까지 주어지면 동일 IP 내에서 정확히 매칭', () => {
  const ct = makeCt([
    makeNode('edge-a', 'NODE', 'ONLINE', '192.168.0.9', 1883, 1),
    makeNode('edge-b', 'NODE', 'ONLINE', '192.168.0.9', 1884, 1),
  ]);
  assert.equal(resolvePrimaryEdgeId(ct, '192.168.0.9', 1884), 'edge-b');
});
