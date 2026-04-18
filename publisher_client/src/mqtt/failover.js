function normalizeBrokerUrl(raw) {
  if (typeof raw !== 'string') return null;

  const trimmed = raw.trim();
  if (!trimmed) return null;

  try {
    const url = new URL(trimmed);
    if (url.protocol !== 'ws:' && url.protocol !== 'wss:') return null;
    return url.toString();
  } catch {
    return null;
  }
}

export function parseBrokerCandidates(rawInput) {
  if (typeof rawInput !== 'string') return [];

  const seen = new Set();
  const brokers = [];
  const parts = rawInput
    .split(/[\n,\s]+/)
    .map(part => normalizeBrokerUrl(part))
    .filter(Boolean);

  parts.forEach((brokerUrl) => {
    if (seen.has(brokerUrl)) return;
    seen.add(brokerUrl);
    brokers.push(brokerUrl);
  });

  return brokers;
}

export function getNextBrokerIndex(currentIndex, total) {
  if (!Number.isInteger(total) || total <= 0) return -1;
  if (!Number.isInteger(currentIndex) || currentIndex < 0) return 0;
  return (currentIndex + 1) % total;
}
