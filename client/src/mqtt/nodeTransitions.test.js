import test from 'node:test';
import assert from 'node:assert/strict';
import {
  RECENT_NODE_TRANSITION_WINDOW_MS,
  getRecentRecoveryDeadline,
  isRecentlyRecoveredNode,
} from './nodeTransitions.js';

test('isRecentlyRecoveredNode returns true only within the recovery highlight window', () => {
  const node = {
    id: 'edge-a',
    role: 'NODE',
    status: 'ONLINE',
    previous_status: 'OFFLINE',
    status_changed_at: '2026-04-19T06:29:58Z',
  };

  const changedAt = Date.parse(node.status_changed_at);

  assert.equal(
    isRecentlyRecoveredNode(node, changedAt + 3000),
    true,
  );
  assert.equal(
    isRecentlyRecoveredNode(node, changedAt + RECENT_NODE_TRANSITION_WINDOW_MS + 100),
    false,
  );
  assert.equal(getRecentRecoveryDeadline(node), changedAt + RECENT_NODE_TRANSITION_WINDOW_MS);
});
