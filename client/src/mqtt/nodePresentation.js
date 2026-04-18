function parseIpv4(ip) {
  if (typeof ip !== 'string') return null;

  const parts = ip.split('.');
  if (parts.length !== 4) return null;

  const numbers = parts.map(part => Number(part));
  if (numbers.some(number => !Number.isInteger(number) || number < 0 || number > 255)) {
    return null;
  }

  return numbers;
}

function compareNodeEndpoints(left, right) {
  const leftIp = parseIpv4(left?.ip);
  const rightIp = parseIpv4(right?.ip);

  if (leftIp && rightIp) {
    for (let i = 0; i < 4; i += 1) {
      if (leftIp[i] !== rightIp[i]) return leftIp[i] - rightIp[i];
    }
  } else if (leftIp) {
    return -1;
  } else if (rightIp) {
    return 1;
  }

  const leftPort = Number(left?.port ?? 0);
  const rightPort = Number(right?.port ?? 0);
  if (leftPort !== rightPort) return leftPort - rightPort;

  return String(left?.id ?? '').localeCompare(String(right?.id ?? ''));
}

function normalizeAlias(alias) {
  return typeof alias === 'string' ? alias.trim() : '';
}

export function getNodeAliasKey(node) {
  if (!node) return '';
  if (node.ip) return `${node.ip}:${node.port ?? ''}`;
  return String(node.id ?? '');
}

export function resolveNodeAlias(node, aliases = {}) {
  if (!node) return '';
  const key = getNodeAliasKey(node);
  return normalizeAlias(aliases[key]) || normalizeAlias(aliases[node.id]);
}

export function buildNodePresentationMap(topology, aliases = {}) {
  const map = new Map();
  const nodes = Array.isArray(topology?.nodes) ? topology.nodes : [];

  nodes
    .filter(node => node?.id && node.role === 'CORE')
    .forEach((node) => {
      let title = 'CORE';
      let graphLabel = 'CORE';

      if (node.id === topology?.active_core_id) {
        title = 'ACTIVE CORE';
        graphLabel = 'ACTIVE\nCORE';
      } else if (node.id === topology?.backup_core_id) {
        title = 'BACKUP CORE';
        graphLabel = 'BACKUP\nCORE';
      }

      const endpoint = node.ip ? `${node.ip}${node.port ? `:${node.port}` : ''}` : '';
      map.set(String(node.id), {
        endpoint,
        graphLabel,
        listLabel: title,
        filterLabel: title,
      });
    });

  const edgeNodes = nodes
    .filter(node => node?.id && node.role !== 'CORE')
    .sort(compareNodeEndpoints);

  edgeNodes.forEach((node, index) => {
    const alias = resolveNodeAlias(node, aliases);
    const edgeNumber = index + 1;
    const edgeLabel = `EDGE ${edgeNumber}`;
    const endpoint = node.ip ? `${node.ip}${node.port ? `:${node.port}` : ''}` : '';

    map.set(String(node.id), {
      edgeNumber,
      edgeLabel,
      alias,
      endpoint,
      graphLabel: alias ? `${edgeLabel}\n${alias}` : edgeLabel,
      listLabel: alias ? `${edgeLabel} · ${alias}` : edgeLabel,
      filterLabel: alias ? `${edgeLabel} · ${alias}` : edgeLabel,
    });
  });

  return map;
}
