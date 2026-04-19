// CT 기반 Failover 알고리즘 — broker/include/publisher_helpers.h의 JS 포팅

function getRtt(links, fromId, toId) {
  if (!Array.isArray(links) || !fromId || !toId) return Infinity;

  const link = links.find(
    l => (l.from_id === fromId && l.to_id === toId) ||
         (l.from_id === toId && l.to_id === fromId)
  );
  return typeof link?.rtt_ms === 'number' ? link.rtt_ms : Infinity;
}

function isBrokerNode(node, role) {
  return node?.role === role &&
    node.status === 'ONLINE' &&
    typeof node.ip === 'string' &&
    node.ip.length > 0 &&
    Number.isFinite(node.port) &&
    node.port > 0;
}

function buildCandidate(node, links, primaryEdgeId) {
  return {
    id: node.id,
    ip: node.ip,
    port: node.port,
    rtt: getRtt(links, primaryEdgeId, node.id),
    hop: Number.isFinite(node.hop_to_core) ? node.hop_to_core : 999,
  };
}

function isBetterCandidate(candidate, best) {
  return !best ||
    candidate.rtt < best.rtt ||
    (candidate.rtt === best.rtt && candidate.hop < best.hop);
}

function isExcludedNode(node, primaryEdgeId, primaryEdgeIp) {
  if (primaryEdgeId && node.id === primaryEdgeId) return true;
  return !primaryEdgeId && primaryEdgeIp && node.ip === primaryEdgeIp;
}

function pickBestCandidate(nodes, links, primaryEdgeId, role, primaryEdgeIp = '') {
  let best = null;

  for (const node of nodes) {
    if (!isBrokerNode(node, role)) continue;
    if (isExcludedNode(node, primaryEdgeId, primaryEdgeIp)) continue;

    const candidate = buildCandidate(node, links, primaryEdgeId);
    if (isBetterCandidate(candidate, best)) {
      best = candidate;
    }
  }

  return best;
}

function toBroker(node) {
  return node ? { id: node.id, ip: node.ip, port: node.port } : null;
}

/**
 * 우선순위 core를 선택한다.
 * active core 우선, 없으면 backup core, 둘 다 없으면 ONLINE core 중 RTT/hop 기준.
 * @param {object} ct - Connection Table ({ nodes, links, active_core_id, backup_core_id })
 * @param {string|null} primaryEdgeId - RTT 계산 기준 edge UUID
 * @returns {{ id: string, ip: string, port: number } | null}
 */
export function findPreferredCoreBroker(ct, primaryEdgeId = null) {
  if (!Array.isArray(ct?.nodes)) return null;

  const activeCore = ct.nodes.find(
    node => node.id === ct.active_core_id && isBrokerNode(node, 'CORE')
  );
  if (activeCore) return toBroker(activeCore);

  const backupCore = ct.nodes.find(
    node => node.id === ct.backup_core_id && isBrokerNode(node, 'CORE')
  );
  if (backupCore) return toBroker(backupCore);

  const bestCore = pickBestCandidate(ct.nodes, ct.links, primaryEdgeId, 'CORE');
  return bestCore ? toBroker(bestCore) : null;
}

/**
 * CT에서 failover 대상 브로커를 선택한다.
 * 우선순위: 다른 Edge(NODE) 중 RTT 최소, 동점 시 hop_to_core 최소.
 * Edge 후보가 없으면 active core → backup core → 기타 ONLINE core 순으로 선택한다.
 * @param {object} ct - Connection Table ({ nodes, links })
 * @param {string|null} primaryEdgeId - 현재 연결된 Edge UUID (제외 대상)
 * @param {string} primaryEdgeIp - 현재 연결된 Edge IP (UUID 미확정 시 제외 대상)
 * @returns {{ found: boolean, id?: string, ip?: string, port?: number }}
 */
export function selectFallbackBroker(ct, primaryEdgeId, primaryEdgeIp = '') {
  if (!Array.isArray(ct?.nodes) || ct.nodes.length === 0) {
    return { found: false };
  }

  const bestEdge = pickBestCandidate(ct.nodes, ct.links, primaryEdgeId, 'NODE', primaryEdgeIp);
  if (bestEdge) {
    return { found: true, id: bestEdge.id, ip: bestEdge.ip, port: bestEdge.port };
  }

  const preferredCore = findPreferredCoreBroker(ct, primaryEdgeId);
  return preferredCore
    ? { found: true, id: preferredCore.id, ip: preferredCore.ip, port: preferredCore.port }
    : { found: false };
}

/**
 * Primary Edge가 CT에서 ONLINE 상태로 복구됐는지 확인한다.
 * @param {object} ct - Connection Table
 * @param {string|null} primaryEdgeId
 * @returns {boolean}
 */
export function shouldReturnToPrimary(ct, primaryEdgeId) {
  if (!primaryEdgeId || !Array.isArray(ct?.nodes)) return false;
  return ct.nodes.some(n => n.id === primaryEdgeId && n.status === 'ONLINE');
}

/**
 * 브라우저 publisher가 현재 CT를 적용해도 되는지 판단한다.
 * 최초 CT는 허용하고, 이후에는 version 증가분만 반영한다.
 * @param {object|null} previousCt
 * @param {object|null} nextCt
 * @returns {boolean}
 */
export function shouldAcceptCtUpdate(previousCt, nextCt) {
  if (!nextCt || typeof nextCt.version !== 'number') return false;
  if (!Array.isArray(nextCt.nodes) || !Array.isArray(nextCt.links)) return false;
  if (typeof nextCt.active_core_id !== 'string') return false;
  if (typeof nextCt.backup_core_id !== 'string') return false;
  if (!previousCt || typeof previousCt.version !== 'number') return true;
  return nextCt.version > previousCt.version;
}

/**
 * 초기 연결 URL의 IP(옵션: port)로 CT에서 Edge UUID를 탐색한다.
 * @param {object} ct - Connection Table
 * @param {string} ip - Edge IP 주소
 * @param {number|null} port - Edge MQTT 포트(알면 함께 비교)
 * @returns {string|null}
 */
export function resolvePrimaryEdgeId(ct, ip, port = null) {
  if (!Array.isArray(ct?.nodes)) return null;

  return ct.nodes.find(node => (
    node.role === 'NODE' &&
    node.ip === ip &&
    (port == null || node.port === port)
  ))?.id ?? null;
}
