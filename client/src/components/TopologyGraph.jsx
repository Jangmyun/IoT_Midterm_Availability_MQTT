import { useEffect, useRef } from 'react';
import cytoscape from 'cytoscape';
import {
  buildTopologyNodeLabel,
  buildTopologyNodePositions,
  classifyTopologyLink,
  classifyTopologyNode,
} from './topologyGraphModel.js';

/**
 * TopologyGraph
 *
 * - Active CORE: 더 크고 밝은 중심형 노드
 * - Backup CORE: 한 단계 차분한 보조 노드
 * - NODE ONLINE: 초록
 * - NODE OFFLINE: 빨강(반투명)
 * - Active Core 링크: 밝은 실선
 * - Backup Core 링크: 청록 점선(standby)
 * - Edge 라벨: RTT(ms)
 * - 노드 클릭 시 onNodeClick(id) 호출
 * - 배경 클릭 시 onNodeClick(null) 호출
 *
 * @param {{ topology: { nodes: any[], links: any[] } | null, onNodeClick?: (id: string | null) => void }} props
 */
export default function TopologyGraph({ topology, onNodeClick }) {
  const containerRef = useRef(null);
  const cyRef = useRef(null);
  const onNodeClickRef = useRef(onNodeClick);
  const resizeRafRef = useRef(null);

  useEffect(() => {
    onNodeClickRef.current = onNodeClick;
  }, [onNodeClick]);

  const fitGraph = () => {
    const cy = cyRef.current;
    if (!cy || cy.destroyed() || cy.elements().length === 0) return;

    cy.resize();
    cy.fit(cy.elements(), 80);
    cy.center(cy.elements());
  };

  useEffect(() => {
    if (!containerRef.current) return;

    const cy = cytoscape({
      container: containerRef.current,
      elements: [],
      boxSelectionEnabled: false,
      autounselectify: false,
      userZoomingEnabled: true,
      userPanningEnabled: true,
      wheelSensitivity: 0.15,
      minZoom: 0.3,
      maxZoom: 2.5,
      pixelRatio: 'auto',
      nodeDimensionsIncludeLabels: true,

      style: [
        {
          selector: 'node',
          style: {
            label: 'data(label)',
            width: 36,
            height: 36,
            shape: 'ellipse',
            'background-color': '#57be5f',
            'border-width': 2,
            'border-color': '#182233',
            color: '#ffffff',
            'font-size': 11,
            'font-weight': 600,
            'text-valign': 'bottom',
            'text-halign': 'center',
            'text-margin-y': 10,
            'text-wrap': 'wrap',
            'text-max-width': 120,
            'text-outline-width': 4,
            'text-outline-color': '#0b1020',
            'shadow-blur': 12,
            'shadow-opacity': 0.28,
            'shadow-color': '#020617',
            'overlay-opacity': 0,
          },
        },
        {
          selector: 'node[role = "CORE"]',
          style: {
            'font-size': 12,
            'font-weight': 700,
            'text-margin-y': 15,
            'text-max-width': 140,
          },
        },
        {
          selector: 'node[nodeKind = "active-core"]',
          style: {
            shape: 'round-octagon',
            width: 76,
            height: 76,
            'background-color': '#6676ff',
            'border-width': 4.5,
            'border-color': '#eef2ff',
            'shadow-color': '#4f5eff',
            'shadow-opacity': 0.5,
            'shadow-blur': 26,
          },
        },
        {
          selector: 'node[nodeKind = "backup-core"]',
          style: {
            shape: 'round-hexagon',
            width: 58,
            height: 58,
            'background-color': '#1e5e67',
            'border-width': 3,
            'border-color': '#88d8d0',
            'shadow-color': '#1aa7a1',
            'shadow-opacity': 0.22,
            'shadow-blur': 16,
            opacity: 0.94,
          },
        },
        {
          selector: 'node[nodeKind = "core"]',
          style: {
            shape: 'round-diamond',
            width: 60,
            height: 60,
            'background-color': '#6c748f',
            'border-width': 3,
            'border-color': '#d7def0',
          },
        },
        {
          selector: 'node[nodeKind = "offline-node"]',
          style: {
            'background-color': '#ef4444',
            'border-color': '#fca5a5',
            opacity: 0.72,
            'shadow-color': '#ef4444',
            'shadow-opacity': 0.2,
          },
        },
        {
          selector: 'node:selected',
          style: {
            'border-width': 4,
            'border-color': '#ffffff',
          },
        },
        {
          selector: 'edge',
          style: {
            width: 3,
            'line-color': '#516176',
            'target-arrow-color': '#516176',
            'curve-style': 'bezier',
            label: 'data(rtt)',
            color: '#cbd5e1',
            'font-size': 10,
            'font-weight': 500,
            'text-background-color': '#0b1020',
            'text-background-opacity': 0.95,
            'text-background-padding': 3,
            'text-border-opacity': 0,
            'overlay-opacity': 0,
          },
        },
        {
          selector: 'edge[edgeKind = "active-link"]',
          style: {
            width: 4.25,
            'line-color': '#aebdff',
            'target-arrow-color': '#aebdff',
            'line-style': 'solid',
            opacity: 0.95,
          },
        },
        {
          selector: 'edge[edgeKind = "backup-link"]',
          style: {
            width: 2.25,
            'line-color': '#52b7b0',
            'target-arrow-color': '#52b7b0',
            'line-style': 'dashed',
            opacity: 0.52,
          },
        },
      ],
    });

    cyRef.current = cy;

    const handleNodeTap = (evt) => {
      const id = evt.target.id();
      onNodeClickRef.current?.(id);
    };

    const handleBackgroundTap = (evt) => {
      if (evt.target === cy) {
        cy.elements().unselect();
        onNodeClickRef.current?.(null);
      }
    };

    cy.on('tap', 'node', handleNodeTap);
    cy.on('tap', handleBackgroundTap);

    const ro = new ResizeObserver(() => {
      if (resizeRafRef.current) cancelAnimationFrame(resizeRafRef.current);
      resizeRafRef.current = requestAnimationFrame(() => {
        fitGraph();
      });
    });

    ro.observe(containerRef.current);

    return () => {
      if (resizeRafRef.current) cancelAnimationFrame(resizeRafRef.current);
      ro.disconnect();
      cy.off('tap', 'node', handleNodeTap);
      cy.off('tap', handleBackgroundTap);
      cy.destroy();
      cyRef.current = null;
    };
  }, []);

  useEffect(() => {
    const cy = cyRef.current;
    if (!cy || cy.destroyed()) return;

    if (!topology || !Array.isArray(topology.nodes) || !Array.isArray(topology.links)) {
      cy.elements().remove();
      return;
    }

    const visibleNodes = topology.nodes.filter((node) => {
      return !(node.role === 'CORE' && node.status === 'OFFLINE');
    });
    const nodePositions = buildTopologyNodePositions(topology, visibleNodes);

    const elements = [];

    for (const n of visibleNodes) {
      const rawId = String(n.id ?? '');
      elements.push({
        group: 'nodes',
        data: {
          id: rawId,
          label: buildTopologyNodeLabel(topology, n),
          role: n.role ?? 'NODE',
          status: n.status ?? 'ONLINE',
          nodeKind: classifyTopologyNode(topology, n),
        },
        position: nodePositions[rawId] ?? { x: 0, y: 0 },
      });
    }

    const nodeIdSet = new Set(visibleNodes.map((n) => String(n.id ?? '')));

    for (const l of topology.links) {
      const fromId = String(l.from_id ?? '');
      const toId = String(l.to_id ?? '');

      if (!nodeIdSet.has(fromId) || !nodeIdSet.has(toId)) continue;

      const rttValue =
        typeof l.rtt_ms === 'number' && Number.isFinite(l.rtt_ms) && l.rtt_ms > 0
          ? `${Math.round(l.rtt_ms)}ms`
          : '';

      elements.push({
        group: 'edges',
        data: {
          id: `${fromId}->${toId}`,
          source: fromId,
          target: toId,
          rtt: rttValue,
          edgeKind: classifyTopologyLink(topology, l),
        },
      });
    }

    cy.batch(() => {
      cy.elements().remove();
      cy.add(elements);
    });

    const layout = cy.layout({
      name: 'preset',
      animate: true,
      animationDuration: 420,
      fit: true,
      padding: 120,
    });

    layout.on('layoutstop', () => {
      requestAnimationFrame(() => {
        fitGraph();
      });
    });

    requestAnimationFrame(() => {
      cy.resize();
      layout.run();
    });
  }, [topology]);

  return (
    <div
      ref={containerRef}
      className="topology-graph"
      style={{
        width: '100%',
        height: '100%',
        minHeight: '420px',
        position: 'relative',
        overflow: 'hidden',
      }}
    />
  );
}
