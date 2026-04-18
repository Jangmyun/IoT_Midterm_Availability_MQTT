function normalizeHiddenNodeIds(hiddenNodeIds) {
  if (hiddenNodeIds instanceof Set) return hiddenNodeIds;
  if (Array.isArray(hiddenNodeIds)) return new Set(hiddenNodeIds);
  return new Set();
}

function isDisplayableNodeStatus(status) {
  return status === 'ONLINE' || status === 'OFFLINE';
}

function endpointKey(node) {
  if (!node?.id) return '';
  if (node.role === 'CORE') return `CORE:${node.id}`;
  if (node.ip && node.port !== undefined && node.port !== null) {
    return `NODE:${node.ip}:${node.port}`;
  }
  return `NODE_ID:${node.id}`;
}

function preferPresentationNode(current, candidate) {
  if (!current) return candidate;

  const currentOnline = current.status === 'ONLINE';
  const candidateOnline = candidate.status === 'ONLINE';

  if (currentOnline !== candidateOnline) {
    return candidateOnline ? candidate : current;
  }

  return current;
}

export function reconcileHiddenNodeIds(topology, hiddenNodeIds = new Set()) {
  const hiddenIds = normalizeHiddenNodeIds(hiddenNodeIds);

  if (!topology || !Array.isArray(topology.nodes)) {
    return new Set(hiddenIds);
  }

  const nextHiddenIds = new Set(hiddenIds);
  for (const node of topology.nodes) {
    if (node?.id && node.status === 'ONLINE') {
      nextHiddenIds.delete(node.id);
    }
  }

  return nextHiddenIds;
}

function isCurrentCoreNode(node, topology) {
  if (!node || node.role !== 'CORE') return false;
  if (node.id === topology.active_core_id) return true;
  if (topology.backup_core_id && node.id === topology.backup_core_id) return true;
  return false;
}

export function buildPresentationTopology(topology, hiddenNodeIds = new Set()) {
  if (!topology || !Array.isArray(topology.nodes) || !Array.isArray(topology.links)) {
    return topology;
  }

  const hiddenIds = normalizeHiddenNodeIds(hiddenNodeIds);

  const visibleCandidates = topology.nodes.filter((node) => {
    if (!node?.id || !isDisplayableNodeStatus(node.status)) return false;
    if (hiddenIds.has(node.id)) return false;

    if (node.role === 'CORE') {
      return isCurrentCoreNode(node, topology) && node.status === 'ONLINE';
    }

    return true;
  });

  const nodeByEndpoint = new Map();
  for (const node of visibleCandidates) {
    const key = endpointKey(node);
    nodeByEndpoint.set(key, preferPresentationNode(nodeByEndpoint.get(key), node));
  }

  const nodes = [...nodeByEndpoint.values()];

  const nodeIds = new Set(nodes.map((node) => node.id));
  const links = topology.links.filter((link) => {
    return nodeIds.has(link.from_id) && nodeIds.has(link.to_id);
  });

  return {
    ...topology,
    nodes,
    links,
  };
}
