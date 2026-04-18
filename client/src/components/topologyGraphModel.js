const EDGE_SLOT_ANGLES = [28, 52, 76, 104, 128, 152, 40, 64, 92, 116, 140];
const TOP_ROW_Y = -150;
const CORE_GAP_X = 160;
const EXTRA_CORE_GAP_X = 140;
const EDGE_CENTER_Y_OFFSET = 40;

function buildRoleMap(nodes) {
  return new Map(
    (Array.isArray(nodes) ? nodes : [])
      .filter((node) => node?.id)
      .map((node) => [String(node.id), node.role ?? 'NODE']),
  );
}

function stableHash(input) {
  let hash = 2166136261;

  for (const ch of String(input ?? '')) {
    hash ^= ch.charCodeAt(0);
    hash = Math.imul(hash, 16777619);
  }

  return hash >>> 0;
}

export function classifyTopologyNode(topology, node) {
  if (!topology || !node?.id) return 'node';

  if (node.role !== 'CORE') {
    return node.status === 'OFFLINE' ? 'offline-node' : 'node';
  }

  if (node.id === topology.active_core_id) {
    return 'active-core';
  }

  if (topology.backup_core_id && node.id === topology.backup_core_id) {
    return 'backup-core';
  }

  return 'core';
}

export function buildTopologyNodeLabel(topology, node) {
  const shortId = String(node?.id ?? '').slice(0, 8);
  if (!shortId) return '';

  if (node?.role === 'CORE') {
    if (node.id === topology?.active_core_id) return `CORE\n${shortId}`;
    if (node.id === topology?.backup_core_id) return `BACKUP\n${shortId}`;
    return `CORE\n${shortId}`;
  }

  return shortId;
}

export function buildTopologyNodePositions(topology, nodes) {
  const safeNodes = (Array.isArray(nodes) ? nodes : []).filter((node) => node?.id);
  const positions = {};

  const activeCore = safeNodes.find((node) => node.id === topology?.active_core_id) ?? null;
  const backupCore = safeNodes.find((node) => node.id === topology?.backup_core_id) ?? null;
  const otherCores = safeNodes.filter((node) => (
    node.role === 'CORE' &&
    node.id !== activeCore?.id &&
    node.id !== backupCore?.id
  ));

  let edgeAnchor = { x: 0, y: TOP_ROW_Y + EDGE_CENTER_Y_OFFSET };

  if (activeCore && backupCore && activeCore.id !== backupCore.id) {
    positions[activeCore.id] = { x: -CORE_GAP_X / 2, y: TOP_ROW_Y };
    positions[backupCore.id] = { x: CORE_GAP_X / 2, y: TOP_ROW_Y };
    edgeAnchor = { x: 0, y: TOP_ROW_Y + EDGE_CENTER_Y_OFFSET };
  } else if (activeCore) {
    positions[activeCore.id] = { x: 0, y: TOP_ROW_Y };
    edgeAnchor = { x: 0, y: TOP_ROW_Y + EDGE_CENTER_Y_OFFSET };
  } else if (backupCore) {
    positions[backupCore.id] = { x: 0, y: TOP_ROW_Y };
    edgeAnchor = { x: 0, y: TOP_ROW_Y + EDGE_CENTER_Y_OFFSET };
  }

  otherCores.forEach((node, index) => {
    const direction = index % 2 === 0 ? -1 : 1;
    const offsetIndex = Math.floor(index / 2) + 1;
    positions[node.id] = {
      x: direction * (CORE_GAP_X / 2 + EXTRA_CORE_GAP_X * offsetIndex),
      y: TOP_ROW_Y + 32,
    };
  });

  const edgeNodes = safeNodes
    .filter((node) => node.role !== 'CORE')
    .sort((left, right) => stableHash(left.id) - stableHash(right.id));

  edgeNodes.forEach((node, index) => {
    const hash = stableHash(node.id);
    const ringIndex = Math.floor(index / EDGE_SLOT_ANGLES.length);
    const slotIndex = index % EDGE_SLOT_ANGLES.length;
    const angleDeg = EDGE_SLOT_ANGLES[slotIndex] + ((hash % 12) - 6);
    const radius =
      230 +
      ringIndex * 112 +
      ((hash >> 3) % 24) +
      (node.status === 'OFFLINE' ? 42 : 0);
    const angle = (angleDeg * Math.PI) / 180;

    positions[node.id] = {
      x: edgeAnchor.x + Math.cos(angle) * radius,
      y: edgeAnchor.y + Math.sin(angle) * radius,
    };
  });

  return positions;
}

export function classifyTopologyLink(topology, link) {
  if (!topology || !link) return 'default-link';

  const fromId = String(link.from_id ?? '');
  const toId = String(link.to_id ?? '');
  const roleMap = buildRoleMap(topology.nodes);

  const fromRole = roleMap.get(fromId) ?? 'NODE';
  const toRole = roleMap.get(toId) ?? 'NODE';

  const connectsCoreToNode = (coreId) => {
    if (!coreId) return false;

    return (
      (fromId === coreId && toRole !== 'CORE') ||
      (toId === coreId && fromRole !== 'CORE')
    );
  };

  if (connectsCoreToNode(topology.active_core_id)) {
    return 'active-link';
  }

  if (connectsCoreToNode(topology.backup_core_id)) {
    return 'backup-link';
  }

  return 'default-link';
}
