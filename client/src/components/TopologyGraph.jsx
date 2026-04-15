import { useEffect, useRef } from 'react';
import cytoscape from 'cytoscape';

/**
 * TopologyGraph
 *
 * ConnectionTable을 Cytoscape 그래프로 시각화한다.
 * - CORE 노드: 보라색 다이아몬드
 * - NODE ONLINE: 초록
 * - NODE OFFLINE: 빨강 (반투명)
 * - Edge 라벨: RTT(ms)
 * - 노드 클릭 시 onNodeClick(id) 호출, 배경 클릭 시 onNodeClick(null)
 *
 * @param {{ topology: object|null, onNodeClick: (id: string|null) => void }} props
 */
export default function TopologyGraph({ topology, onNodeClick }) {
  const containerRef   = useRef(null);
  const cyRef          = useRef(null);
  // onNodeClick을 ref로 보관 — init effect 클로저 내에서 최신 콜백 참조
  const onNodeClickRef = useRef(onNodeClick);
  useEffect(() => { onNodeClickRef.current = onNodeClick; }, [onNodeClick]);

  // Cytoscape 인스턴스 초기화 (마운트 1회)
  useEffect(() => {
    if (!containerRef.current) return;

    cyRef.current = cytoscape({
      container: containerRef.current,
      style: [
        {
          selector: 'node',
          style: {
            'background-color': '#4caf50',
            label: 'data(label)',
            'font-size': 10,
            color: '#ffffff',
            'text-valign': 'bottom',
            'text-margin-y': 4,
            width: 28,
            height: 28,
          },
        },
        {
          selector: 'node[role = "CORE"]',
          style: {
            'background-color': '#6366f1',
            shape: 'diamond',
            width: 44,
            height: 44,
          },
        },
        {
          selector: 'node[status = "OFFLINE"]',
          style: {
            'background-color': '#f44336',
            opacity: 0.55,
          },
        },
        {
          selector: 'node:selected',
          style: {
            'border-width': 3,
            'border-color': '#fff',
            'border-opacity': 0.9,
          },
        },
        {
          selector: 'edge',
          style: {
            'line-color': '#3a3a5a',
            width: 2,
            label: 'data(rtt)',
            'font-size': 9,
            color: '#888',
            'text-background-color': '#0d0d1a',
            'text-background-opacity': 1,
            'text-background-padding': '2px',
            'curve-style': 'bezier',
          },
        },
      ],
      layout: { name: 'cose' },
      elements: [],
      userZoomingEnabled: true,
      userPanningEnabled: true,
    });

    // 노드 클릭
    cyRef.current.on('tap', 'node', evt => {
      onNodeClickRef.current?.(evt.target.id());
    });
    // 배경 클릭 → 선택 해제
    cyRef.current.on('tap', evt => {
      if (evt.target === cyRef.current) onNodeClickRef.current?.(null);
    });

    return () => {
      cyRef.current?.destroy();
      cyRef.current = null;
    };
  }, []);

  // topology 변경 시 그래프 업데이트 (인스턴스 재생성 없이)
  useEffect(() => {
    const cy = cyRef.current;
    if (!cy || !topology) return;

    cy.elements().remove();

    topology.nodes.forEach(n => {
      cy.add({
        group: 'nodes',
        data: {
          id:     n.id,
          label:  n.id.slice(0, 6),
          role:   n.role,
          status: n.status,
        },
      });
    });

    topology.links.forEach(l => {
      // 양쪽 노드가 그래프에 존재할 때만 edge 추가
      if (cy.getElementById(l.from_id).length && cy.getElementById(l.to_id).length) {
        cy.add({
          group: 'edges',
          data: {
            source: l.from_id,
            target: l.to_id,
            rtt:    `${l.rtt_ms.toFixed(0)}ms`,
          },
        });
      }
    });

    cy.layout({ name: 'cose', animate: false, randomize: false }).run();
    cy.style().update();
  }, [topology]);

  return (
    <div
      ref={containerRef}
      className="topology-graph"
    />
  );
}
