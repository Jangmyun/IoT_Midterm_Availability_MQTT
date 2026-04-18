function normalizeHiddenNodeIds(hiddenNodeIds) {
  if (hiddenNodeIds instanceof Set) return hiddenNodeIds;
  if (Array.isArray(hiddenNodeIds)) return new Set(hiddenNodeIds);
  return new Set();
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

  const nodes = topology.nodes.filter((node) => {
    if (!node?.id || node.status !== 'ONLINE') return false;

    if (node.role === 'CORE') {
      return isCurrentCoreNode(node, topology);
    }

    return !hiddenIds.has(node.id);
  });

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
