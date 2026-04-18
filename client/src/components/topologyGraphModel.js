function buildRoleMap(nodes) {
  return new Map(
    (Array.isArray(nodes) ? nodes : [])
      .filter((node) => node?.id)
      .map((node) => [String(node.id), node.role ?? 'NODE']),
  );
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
