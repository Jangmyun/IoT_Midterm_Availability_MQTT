export const RECENT_NODE_TRANSITION_WINDOW_MS = 15000;

function parseTimestampMs(rawTimestamp) {
  const parsed = Date.parse(rawTimestamp ?? '');
  return Number.isFinite(parsed) ? parsed : null;
}

export function getRecentRecoveryDeadline(node, windowMs = RECENT_NODE_TRANSITION_WINDOW_MS) {
  if (!node || node.role === 'CORE') return null;
  if (node.status !== 'ONLINE' || node.previous_status !== 'OFFLINE') return null;

  const changedAt = parseTimestampMs(node.status_changed_at);
  if (changedAt === null) return null;

  return changedAt + windowMs;
}

export function isRecentlyRecoveredNode(
  node,
  now = Date.now(),
  windowMs = RECENT_NODE_TRANSITION_WINDOW_MS,
) {
  const deadline = getRecentRecoveryDeadline(node, windowMs);
  if (deadline === null) return false;

  return now <= deadline;
}
