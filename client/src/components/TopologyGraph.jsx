import { useEffect, useRef } from 'react';
import cytoscape from 'cytoscape';

/**
 * TopologyGraph
 *
 * - CORE 노드: 보라색 다이아몬드
 * - NODE ONLINE: 초록
 * - NODE OFFLINE: 빨강(반투명)
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
            width: 34,
            height: 34,
            'background-color': '#4caf50',
            'border-width': 2,
            'border-color': '#1f2937',
            color: '#ffffff',
            'font-size': 12,
            'font-weight': 600,
            'text-valign': 'bottom',
            'text-halign': 'center',
            'text-margin-y': 8,
            'text-wrap': 'none',
            'text-max-width': 120,
            'overlay-opacity': 0,
          },
        },
        {
          selector: 'node[role = "CORE"]',
          style: {
            shape: 'diamond',
            width: 52,
            height: 52,
            'background-color': '#6366f1',
            'border-width': 3,
            'border-color': '#a5b4fc',
          },
        },
        {
          selector: 'node[status = "OFFLINE"]',
          style: {
            'background-color': '#ef4444',
            opacity: 0.6,
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
            'line-color': '#4b5563',
            'target-arrow-color': '#4b5563',
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

    const elements = [];

    for (const n of visibleNodes) {
      const rawId = String(n.id ?? '');
      elements.push({
        group: 'nodes',
        data: {
          id: rawId,
          label: rawId.slice(0, 8),
          role: n.role ?? 'NODE',
          status: n.status ?? 'ONLINE',
        },
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
        },
      });
    }

    cy.batch(() => {
      cy.elements().remove();
      cy.add(elements);
    });

    const nodeCount = visibleNodes.length;

    // 작은 토폴로지는 concentric이 가장 안정적으로 "가운데" 잘 잡힘
    // 큰 토폴로지는 cose로 자연스럽게 퍼뜨림
    const layoutOptions =
      nodeCount <= 20
        ? {
            name: 'concentric',
            animate: false,
            fit: true,
            padding: 100,
            avoidOverlap: true,
            spacingFactor: 1.15,
            minNodeSpacing: 60,
            startAngle: -Math.PI / 2,
            concentric: (node) => {
              const role = node.data('role');
              const status = node.data('status');

              if (role === 'CORE') return 3;
              if (status === 'OFFLINE') return 1;
              return 2;
            },
            levelWidth: () => 1,
          }
        : {
            name: 'cose',
            animate: false,
            fit: true,
            padding: 100,
            randomize: false,
            nodeRepulsion: 120000,
            idealEdgeLength: 160,
            edgeElasticity: 120,
            nestingFactor: 1.1,
            gravity: 0.25,
            numIter: 1200,
          };

    const layout = cy.layout(layoutOptions);

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
