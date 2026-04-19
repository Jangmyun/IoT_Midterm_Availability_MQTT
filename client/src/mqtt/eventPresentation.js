function normalizeDescription(rawDescription) {
  if (typeof rawDescription !== 'string') return '';

  const trimmed = rawDescription.trim();
  if (!trimmed) return '';
  if (trimmed === 'auto-detect') return 'Camera auto-detect';
  if (trimmed === 'manual') return 'Manual trigger';
  if (trimmed.startsWith('{')) return '';
  return trimmed;
}

function normalizeLocationText(value) {
  return typeof value === 'string' ? value.trim() : '';
}

function safeParseJson(raw) {
  try {
    return JSON.parse(raw);
  } catch {
    return null;
  }
}

export function shortId(id) {
  if (!id) return '';
  return `${id.slice(0, 8)}…`;
}

export function formatEventTime(timestamp) {
  if (typeof timestamp !== 'string') return '—';

  const match = timestamp.match(/T(\d{2}:\d{2}:\d{2})/);
  if (match?.[1]) return match[1];

  return timestamp.slice(11, 19) || '—';
}

export function formatClockTime(value) {
  if (!value) return '—';

  const date = value instanceof Date ? value : new Date(value);
  if (Number.isNaN(date.getTime())) return '—';

  return date.toLocaleTimeString('ko-KR', {
    hour12: false,
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
}

export function formatDelayLabel(delayMs) {
  if (!Number.isFinite(delayMs) || delayMs < 1000) return 'just now';

  const totalSeconds = Math.round(delayMs / 1000);
  const minutes = Math.floor(totalSeconds / 60);
  const seconds = totalSeconds % 60;

  if (minutes === 0) {
    return `${seconds}s late`;
  }
  if (seconds === 0) {
    return `${minutes}m late`;
  }
  return `${minutes}m ${seconds}s late`;
}

export function parseNestedPublisherMessage(rawDescription) {
  if (typeof rawDescription !== 'string' || rawDescription.trim() === '') {
    return null;
  }

  const parsed = safeParseJson(rawDescription);
  if (!parsed || typeof parsed !== 'object') return null;

  return {
    publisherId:    typeof parsed.source?.id === 'string' ? parsed.source.id : '',
    description:    normalizeDescription(parsed.payload?.description),
    viaFailover:    parsed.via_failover ?? false,
    originalEdgeId: typeof parsed.original_edge_id === 'string'
      ? parsed.original_edge_id
      : (typeof parsed.intended_edge_id === 'string' ? parsed.intended_edge_id : ''),
    originalEdgeIp: typeof parsed.original_edge_ip === 'string'
      ? parsed.original_edge_ip
      : (typeof parsed.intended_edge_ip === 'string' ? parsed.intended_edge_ip : ''),
    intendedEdgeIp: typeof parsed.intended_edge_ip === 'string' ? parsed.intended_edge_ip : '',
    wasQueued:      parsed.was_queued ?? false,
    createdAt:      typeof parsed.created_at === 'string' ? parsed.created_at : '',
  };
}

function findNodeByIp(nodeById, ip) {
  if (!ip || !nodeById) return null;

  for (const node of nodeById.values()) {
    if (node.ip === ip) return node;
  }

  return null;
}

function buildEdgeLabel(nodeId, node, nodeDisplayMap, fallbackIp = '') {
  const display = nodeId ? nodeDisplayMap?.get(nodeId) ?? null : null;
  if (display?.edgeLabel) return display.edgeLabel;
  if (fallbackIp || node?.ip) return `EDGE ${fallbackIp || node.ip}`;
  return nodeId ? shortId(nodeId) : 'EDGE';
}

export function formatEventSourceOption(nodeId, node, nodeDisplayMap) {
  const display = nodeDisplayMap?.get(nodeId);
  if (display?.filterLabel) return display.filterLabel;

  const label = shortId(nodeId);
  if (node?.ip) return node.ip;
  return label || 'Unknown edge';
}

export function getEventPresentation(event, nodeById, nodeDisplayMap) {
  const actualSourceId = event?.source?.id ?? '';
  const actualSourceNode = actualSourceId ? nodeById?.get(actualSourceId) ?? null : null;
  const actualSourceDisplay = actualSourceId ? nodeDisplayMap?.get(actualSourceId) ?? null : null;
  const nested = parseNestedPublisherMessage(event?.payload?.description);
  const buildingId = event?.payload?.building_id ?? '';
  const cameraId = event?.payload?.camera_id ?? '';
  const routeOriginalId = typeof event?.route?.original_node === 'string' ? event.route.original_node : '';

  const locationLabel = [buildingId, cameraId].filter(Boolean).join(' / ') || 'Location unavailable';
  const automaticSourceLabel = normalizeLocationText(buildingId) || normalizeLocationText(cameraId);
  const routeOriginalNode =
    routeOriginalId && routeOriginalId !== actualSourceId
      ? nodeById?.get(routeOriginalId) ?? null
      : null;
  const metadataOriginalNode =
    !routeOriginalNode && nested?.originalEdgeId && nested.originalEdgeId !== actualSourceId
      ? nodeById?.get(nested.originalEdgeId) ?? null
      : null;
  const metadataOriginalIpNode =
    !routeOriginalNode && !metadataOriginalNode && nested?.originalEdgeIp
      ? findNodeByIp(nodeById, nested.originalEdgeIp)
      : null;

  const displaySourceNode = routeOriginalNode ?? metadataOriginalNode ?? metadataOriginalIpNode ?? actualSourceNode;
  const hintedSourceId = routeOriginalId || nested?.originalEdgeId || actualSourceId;
  const displaySourceId = displaySourceNode?.id ?? hintedSourceId;
  const displaySourceDisplay = displaySourceId ? nodeDisplayMap?.get(displaySourceId) ?? null : null;
  const sourceIp = displaySourceNode?.ip ?? nested?.originalEdgeIp ?? '';
  const sourcePort = displaySourceNode?.port ? String(displaySourceNode.port) : '';
  const sourceEndpoint = sourceIp ? `${sourceIp}${sourcePort ? `:${sourcePort}` : ''}` : '';
  const actualSourceIp = actualSourceNode?.ip ?? '';
  const actualSourcePort = actualSourceNode?.port ? String(actualSourceNode.port) : '';
  const actualSourceEndpoint = actualSourceIp
    ? `${actualSourceIp}${actualSourcePort ? `:${actualSourcePort}` : ''}`
    : '';
  const viaDifferentNode = Boolean(displaySourceId && actualSourceId && displaySourceId !== actualSourceId);

  let queueDelayMs = 0;
  if (nested?.wasQueued && nested?.createdAt && event?.timestamp) {
    queueDelayMs = Math.max(0,
      new Date(event.timestamp).getTime() - new Date(nested.createdAt).getTime()
    );
  }

  return {
    sourceId: displaySourceId || actualSourceId,
    actualSourceId,
    sourceIp,
    sourcePort,
    sourceEndpoint,
    actualSourceEndpoint,
    edgeLabel: displaySourceDisplay?.edgeLabel ?? buildEdgeLabel(displaySourceId, displaySourceNode, nodeDisplayMap, sourceIp),
    sourceTitle: displaySourceDisplay?.alias || automaticSourceLabel || displaySourceDisplay?.edgeLabel || sourceIp || shortId(displaySourceId || actualSourceId) || 'Unknown edge',
    sourceAlias: displaySourceDisplay?.alias ?? automaticSourceLabel,
    locationLabel,
    descriptionLabel: nested?.description ?? normalizeDescription(event?.payload?.description),
    publisherId: nested?.publisherId ?? '',
    viaFailover: (nested?.viaFailover ?? false) || viaDifferentNode,
    intendedEdgeLabel: viaDifferentNode
      ? (displaySourceDisplay?.edgeLabel ?? buildEdgeLabel(displaySourceId, displaySourceNode, nodeDisplayMap, sourceIp))
      : '',
    transitEdgeLabel: viaDifferentNode
      ? (actualSourceDisplay?.edgeLabel ?? buildEdgeLabel(actualSourceId, actualSourceNode, nodeDisplayMap, actualSourceIp))
      : '',
    wasQueued: nested?.wasQueued ?? false,
    queueDelayMs,
  };
}
