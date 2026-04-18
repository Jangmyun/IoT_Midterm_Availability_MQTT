const DEFAULT_WS_PORT = '9001';
const DEFAULT_LOCAL_BROKER_URL = `ws://localhost:${DEFAULT_WS_PORT}`;
const LOCAL_HOST_ALIASES = new Set(['localhost', '127.0.0.1']);

export function formatBrokerHost(rawHost) {
  if (typeof rawHost !== 'string') return '';

  const host = rawHost.trim();
  if (!host) return '';
  if (host.startsWith('[') && host.endsWith(']')) return host;

  return host.includes(':') ? `[${host}]` : host;
}

export function resolveInitialBrokerUrl(configuredUrl, locationLike = null) {
  if (typeof configuredUrl === 'string' && configuredUrl.trim()) {
    return configuredUrl.trim();
  }

  const pageHost = typeof locationLike?.hostname === 'string'
    ? locationLike.hostname.trim()
    : '';
  if (!pageHost) {
    return DEFAULT_LOCAL_BROKER_URL;
  }

  const protocol = locationLike?.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${formatBrokerHost(pageHost)}:${DEFAULT_WS_PORT}`;
}

export function parseBrokerUrl(rawUrl) {
  try {
    const url = new URL(rawUrl);
    return {
      protocol: url.protocol || 'ws:',
      host: url.hostname || '',
      port: url.port || '',
    };
  } catch {
    return null;
  }
}

export function parseBrokerEndpoint(rawEndpoint) {
  if (typeof rawEndpoint !== 'string') return null;

  const trimmed = rawEndpoint.trim();
  if (!trimmed) return null;

  if (trimmed.startsWith('[')) {
    const closingIndex = trimmed.indexOf(']');
    if (closingIndex === -1) return null;
    return {
      host: trimmed.slice(1, closingIndex),
      port: trimmed.slice(closingIndex + 2) || '',
    };
  }

  const lastColonIndex = trimmed.lastIndexOf(':');
  if (lastColonIndex === -1) {
    return { host: trimmed, port: '' };
  }

  return {
    host: trimmed.slice(0, lastColonIndex),
    port: trimmed.slice(lastColonIndex + 1),
  };
}

export function sameBrokerHost(left, right) {
  if (typeof left !== 'string' || typeof right !== 'string') return false;

  const normalizedLeft = left.trim().toLowerCase();
  const normalizedRight = right.trim().toLowerCase();
  if (!normalizedLeft || !normalizedRight) return false;
  if (normalizedLeft === normalizedRight) return true;

  return LOCAL_HOST_ALIASES.has(normalizedLeft) && LOCAL_HOST_ALIASES.has(normalizedRight);
}

export function buildBrokerUrl(currentUrl, nextHost, nextPort) {
  if (typeof nextHost !== 'string' || nextHost.trim() === '') return null;

  const parsed = parseBrokerUrl(currentUrl);
  const protocol = parsed?.protocol || 'ws:';
  const port = String(nextPort || parsed?.port || DEFAULT_WS_PORT);
  const host = formatBrokerHost(nextHost);

  return `${protocol}//${host}:${port}`;
}

export function findNodeById(nodes, nodeId) {
  if (!Array.isArray(nodes) || !nodeId) return null;
  return nodes.find(node => node?.id === nodeId) ?? null;
}

export function findBackupCoreNode(topology) {
  return findNodeById(topology?.nodes, topology?.backup_core_id);
}

export function findActiveCoreNode(topology) {
  return findNodeById(topology?.nodes, topology?.active_core_id);
}

export function resolveBackupReconnectTarget(topology, fallbackNode = null) {
  const backupNode = findBackupCoreNode(topology);
  if (backupNode?.ip && backupNode.status !== 'OFFLINE') {
    return backupNode;
  }

  if (fallbackNode?.ip && fallbackNode.status !== 'OFFLINE') {
    return fallbackNode;
  }

  return null;
}

export function selectPromotedActiveNode(previousTopology, nextTopology, currentUrl) {
  if (!previousTopology || !nextTopology) return null;
  if (previousTopology.active_core_id === nextTopology.active_core_id) return null;

  const activeNode = findActiveCoreNode(nextTopology);
  if (!activeNode?.ip || activeNode.status === 'OFFLINE') return null;

  const current = parseBrokerUrl(currentUrl);
  if (current?.host && sameBrokerHost(current.host, activeNode.ip)) {
    return null;
  }

  return activeNode;
}
