function normalizeDescription(rawDescription) {
  if (typeof rawDescription !== 'string') return '';

  const trimmed = rawDescription.trim();
  if (!trimmed) return '';
  if (trimmed === 'auto-detect') return 'Camera auto-detect';
  if (trimmed === 'manual') return 'Manual trigger';
  if (trimmed.startsWith('{') && trimmed.endsWith('}')) return '';
  return trimmed;
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
    publisherId: typeof parsed.source?.id === 'string' ? parsed.source.id : '',
    description: normalizeDescription(parsed.payload?.description),
  };
}

export function formatEventSourceOption(nodeId, node, nodeDisplayMap) {
  const display = nodeDisplayMap?.get(nodeId);
  if (display?.filterLabel) return display.filterLabel;

  const label = shortId(nodeId);
  if (node?.ip) return node.ip;
  return label || 'Unknown edge';
}

export function getEventPresentation(event, nodeById, nodeDisplayMap) {
  const sourceId = event?.source?.id ?? '';
  const sourceNode = sourceId ? nodeById?.get(sourceId) ?? null : null;
  const sourceDisplay = sourceId ? nodeDisplayMap?.get(sourceId) ?? null : null;
  const nested = parseNestedPublisherMessage(event?.payload?.description);
  const buildingId = event?.payload?.building_id ?? '';
  const cameraId = event?.payload?.camera_id ?? '';

  const locationLabel = [buildingId, cameraId].filter(Boolean).join(' / ') || 'Location unavailable';
  const sourceIp = sourceNode?.ip ?? '';
  const sourcePort = sourceNode?.port ? String(sourceNode.port) : '';

  return {
    sourceId,
    sourceIp,
    sourcePort,
    sourceEndpoint: sourceIp ? `${sourceIp}${sourcePort ? `:${sourcePort}` : ''}` : '',
    edgeLabel: sourceDisplay?.edgeLabel ?? (sourceIp ? `EDGE ${sourceIp}` : 'EDGE'),
    sourceTitle: sourceDisplay?.alias || sourceDisplay?.edgeLabel || sourceIp || shortId(sourceId) || 'Unknown edge',
    sourceAlias: sourceDisplay?.alias ?? '',
    locationLabel,
    descriptionLabel: nested?.description ?? normalizeDescription(event?.payload?.description),
  };
}
