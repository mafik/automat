// D3.js Force-Directed Graph Visualization
// This script expects a 'graph' variable to be defined globally with node data

// Wait for DOM to be ready and d3 to be available
document.addEventListener('DOMContentLoaded', function () {
  // Check if graph data exists
  if (typeof graph === 'undefined') {
    console.warn('Graph data not found');
    return;
  }

  // Load d3.js if not already loaded
  if (typeof d3 === 'undefined') {
    const script = document.createElement('script');
    script.src = 'https://d3js.org/d3.v7.min.js';
    script.onload = initializeGraph;
    document.head.appendChild(script);
  } else {
    initializeGraph();
  }

  function initializeGraph() {
    // Graph dimensions and settings
    const width = 600;
    const height = 600;
    const nodeRadius = 3;

    // Create container div anchored to upper right corner
    const graphContainer = d3.select('body')
      .append('div')
      .attr('id', 'graph-container')
      .style('position', 'fixed')
      .style('top', '20px')
      .style('right', '20px')
      .style('z-index', '1000')
      .style('background', 'rgba(255, 255, 255, 0.9)')
      .style('border', '1px solid #ccc')
      .style('border-radius', '5px')
      .style('padding', '10px')
      .style('box-shadow', '0 2px 10px rgba(0,0,0,0.1)');

    // Add close button (X) to the container
    const closeButton = graphContainer
      .append('div')
      .style('position', 'absolute')
      .style('top', '5px')
      .style('right', '5px')
      .style('width', '20px')
      .style('height', '20px')
      .style('background', 'rgba(255, 255, 255, 0.8)')
      .style('border', '1px solid #ccc')
      .style('border-radius', '50%')
      .style('cursor', 'pointer')
      .style('display', 'flex')
      .style('align-items', 'center')
      .style('justify-content', 'center')
      .style('font-size', '12px')
      .style('font-weight', 'bold')
      .style('color', '#666')
      .style('user-select', 'none')
      .text('×')
      .on('mouseover', function () {
        d3.select(this).style('background', 'rgba(255, 0, 0, 0.1)').style('color', '#ff0000');
      })
      .on('mouseout', function () {
        d3.select(this).style('background', 'rgba(255, 255, 255, 0.8)').style('color', '#666');
      })
      .on('click', function () {
        hideGraph();
      });

    // Create SVG element
    const svg = graphContainer
      .append('svg')
      .attr('width', width)
      .attr('height', height);

    // Create show button (initially hidden)
    const showButton = d3.select('body')
      .append('div')
      .attr('id', 'graph-show-button')
      .style('position', 'fixed')
      .style('top', '20px')
      .style('right', '20px')
      .style('z-index', '1000')
      .style('background', 'rgba(255, 255, 255, 0.9)')
      .style('border', '1px solid #ccc')
      .style('border-radius', '5px')
      .style('padding', '10px 15px')
      .style('box-shadow', '0 2px 10px rgba(0,0,0,0.1)')
      .style('cursor', 'pointer')
      .style('font-size', '14px')
      .style('color', '#666')
      .style('user-select', 'none')
      .style('display', 'none')
      .text('Show Map')
      .on('mouseover', function () {
        d3.select(this).style('background', 'rgba(240, 240, 240, 0.9)');
      })
      .on('mouseout', function () {
        d3.select(this).style('background', 'rgba(255, 255, 255, 0.9)');
      })
      .on('click', function () {
        showGraph();
      });

    // Toggle functions
    function hideGraph() {
      graphContainer.style('display', 'none');
      showButton.style('display', 'block');
    }

    function showGraph() {
      graphContainer.style('display', 'block');
      showButton.style('display', 'none');
    }

    // Create a group element for all graph elements that can be transformed
    const graphGroup = svg.append('g')
      .attr('class', 'graph-group');

    // Add zoom/pan behavior to the SVG
    const zoom = d3.zoom()
      .scaleExtent([0.5, 3]) // Allow some zooming as well as panning
      .on('zoom', function (event) {
        graphGroup.attr('transform', event.transform);
      });

    svg.call(zoom);

    // Create arrow marker for edges
    svg.append('defs').append('marker')
      .attr('id', 'arrowhead')
      .attr('viewBox', '0 -5 10 10')
      .attr('refX', 10 + nodeRadius + 1)
      .attr('refY', 0)
      .attr('markerWidth', 6)
      .attr('markerHeight', 6)
      .attr('orient', 'auto')
      .append('path')
      .attr('d', 'M0,-5L10,0L0,5')
      .attr('fill', 'rgba(0,0,0,0.2)');

    // Process graph data to create nodes and links
    const nodes = graph.map(d => ({ ...d }));
    const links = [];

    // Create links from steps_after relationships
    graph.forEach(source => {
      source.steps_after.forEach(targetId => {
        links.push({
          source: source.id,
          target: targetId
        });
      });
    });


    // Custom vertical neighbor force
    function forceVerticalNeighbors() {
      const nodeMap = new Map(nodes.map(n => [n.id, n]));
      const minSeparation = 30; // Minimum vertical separation between connected nodes

      return function (alpha) {
        nodes.forEach(node => {
          nodes.forEach(otherNode => {
            if (node.id === otherNode.id) return;
            const dx = node.x - otherNode.x;
            const dy = node.y - otherNode.y;
            const distance2 = dx * dx + dy * dy;
            if (distance2 < 0.001) return;
            const normal_x = dx / Math.sqrt(distance2);
            const normal_y = dy / Math.sqrt(distance2);
            let force = alpha / distance2 * 80;
            force = Math.min(force, 10000);
            node.vx += normal_x * force;
            node.vy += normal_y * force;
            otherNode.vx -= normal_x * force;
            otherNode.vy -= normal_y * force;
          });

          const margin = 50 * alpha;
          const marginStrength = 0.05;
          if (node.x < margin) {
            node.vx += (margin - node.x) * marginStrength * alpha;
          }
          if (node.x > width - margin) {
            node.vx -= (node.x - (width - margin)) * marginStrength * alpha;
          }
          if (node.y < margin) {
            node.vy += (margin - node.y) * marginStrength * alpha;
          }
          if (node.y > height - margin) {
            node.vy -= (node.y - (height - margin)) * marginStrength * alpha;
          }

          // Push down from steps_before nodes (nodes that should be above this one)
          const befores = graph[node.id].steps_before;
          befores.forEach(before => {
            const beforeNode = nodeMap.get(before);
            if (beforeNode) {
              const dy = node.y - beforeNode.y;
              if (dy < minSeparation) {
                const force = (minSeparation - dy) * alpha * 0.1;
                node.vy += force; // Push this node down
                beforeNode.vy -= force; // Pull the before node up
              }
            }
          });
        });
      };
    }

    // Create the vertical neighbor force
    const verticalForce = forceVerticalNeighbors();

    // Create force simulation
    const simulation = d3.forceSimulation(nodes)
      .force('link', d3.forceLink(links).id(d => d.id).distance(10).strength(0.02))
      .force('center', d3.forceCenter(width / 2, height / 2))
      .force('vertical', verticalForce); // Add custom vertical force

    // Create links (edges) - now added to the graphGroup
    const link = graphGroup.append('g')
      .attr('class', 'links')
      .selectAll('line')
      .data(links)
      .enter().append('line')
      .attr('stroke', 'rgba(0,0,0,0.2)')
      .attr('stroke-width', 1.5)
      .attr('marker-end', 'url(#arrowhead)');

    // Create nodes - now added to the graphGroup
    const node = graphGroup.append('g')
      .attr('class', 'nodes')
      .selectAll('circle')
      .data(nodes)
      .enter().append('circle')
      .attr('r', nodeRadius)
      .attr('fill', '#4A90E2')
      .attr('stroke', '#2E5C8A')
      .attr('stroke-width', 2)
      .style('cursor', 'pointer')
      .call(d3.drag()
        .on('start', dragstarted)
        .on('drag', dragged)
        .on('end', dragended))
      .on('click', function (event, d) {
        // Prevent the click from triggering pan behavior
        event.stopPropagation();
        // Scroll to the corresponding page fragment
        const element = document.getElementById(d.id.toString());
        if (element) {
          element.scrollIntoView({ behavior: 'smooth' });
        }
      });

    // Add hover effects
    node.on('mouseover', function (event, d) {
      d3.select(this).attr('fill', '#6BB6FF');

      // Show tooltip
      const tooltip = d3.select('body').append('div')
        .attr('class', 'graph-tooltip')
        .style('position', 'absolute')
        .style('background', 'rgba(0,0,0,0.8)')
        .style('color', 'white')
        .style('padding', '5px 10px')
        .style('border-radius', '3px')
        .style('font-size', '12px')
        .style('pointer-events', 'none')
        .style('z-index', '1001')
        .text(d.name);

      tooltip.style('left', (event.pageX + 10) + 'px')
        .style('top', (event.pageY - 10) + 'px');
    })
      .on('mouseout', function () {
        d3.select(this).attr('fill', '#4A90E2');
        d3.selectAll('.graph-tooltip').remove();
      });

    // Update positions on simulation tick
    simulation.on('tick', function () {
      link
        .attr('x1', d => d.source.x)
        .attr('y1', d => d.source.y)
        .attr('x2', d => d.target.x)
        .attr('y2', d => d.target.y);

      node
        .attr('cx', d => d.x)
        .attr('cy', d => d.y);
    });

    simulation.alphaTarget(1).restart();
    simulation.tick(5000);

    // Drag functions for individual nodes
    function dragstarted(event, d) {
      d.fx = d.x;
      d.fy = d.y;
    }

    function dragged(event, d) {
      d.fx = event.x;
      d.fy = event.y;
    }

    function dragended(event, d) {
      d.fx = null;
      d.fy = null;
    }
  }
});