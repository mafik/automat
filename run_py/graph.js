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
    const width = 800;
    const height = 600;
    const nodeRadius = 3;

    // Create SVG container anchored to upper right corner
    const svg = d3.select('body')
      .append('div')
      .style('position', 'fixed')
      .style('top', '20px')
      .style('right', '20px')
      .style('z-index', '1000')
      .style('background', 'rgba(255, 255, 255, 0.9)')
      .style('border', '1px solid #ccc')
      .style('border-radius', '5px')
      .style('padding', '10px')
      .style('box-shadow', '0 2px 10px rgba(0,0,0,0.1)')
      .append('svg')
      .attr('width', width)
      .attr('height', height);

    // Create arrow marker for edges
    svg.append('defs').append('marker')
      .attr('id', 'arrowhead')
      .attr('viewBox', '0 -5 10 10')
      .attr('refX', 20)
      .attr('refY', 0)
      .attr('markerWidth', 6)
      .attr('markerHeight', 6)
      .attr('orient', 'auto')
      .append('path')
      .attr('d', 'M0,-5L10,0L0,5')
      .attr('fill', 'rgba(0,0,0,0.6)');

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

    // Create force simulation
    const simulation = d3.forceSimulation(nodes)
      .force('link', d3.forceLink(links).id(d => d.id).distance(10))
      .force('charge', d3.forceManyBody().strength(-30))
      .force('center', d3.forceCenter(width / 2, height / 2));
    // .force('collision', d3.forceCollide().radius(nodeRadius + 2));

    // Create links (edges)
    const link = svg.append('g')
      .attr('class', 'links')
      .selectAll('line')
      .data(links)
      .enter().append('line')
      .attr('stroke', 'rgba(0,0,0,0.6)')
      .attr('stroke-width', 1.5)
      .attr('marker-end', 'url(#arrowhead)');

    // Create nodes
    const node = svg.append('g')
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
      // Constrain nodes to stay within SVG bounds
      // node.each(function (d) {
      //   d.x = Math.max(nodeRadius, Math.min(width - nodeRadius, d.x));
      //   d.y = Math.max(nodeRadius, Math.min(height - nodeRadius, d.y));
      // });

      link
        .attr('x1', d => d.source.x)
        .attr('y1', d => d.source.y)
        .attr('x2', d => d.target.x)
        .attr('y2', d => d.target.y);

      node
        .attr('cx', d => d.x)
        .attr('cy', d => d.y);
    });

    // Drag functions
    function dragstarted(event, d) {
      if (!event.active) simulation.alphaTarget(0.3).restart();
      d.fx = d.x;
      d.fy = d.y;
    }

    function dragged(event, d) {
      d.fx = event.x;
      d.fy = event.y;
    }

    function dragended(event, d) {
      if (!event.active) simulation.alphaTarget(0);
      d.fx = null;
      d.fy = null;
    }

    // Add a title to the graph
    svg.append('text')
      .attr('x', width / 2)
      .attr('y', 15)
      .attr('text-anchor', 'middle')
      .style('font-size', '14px')
      .style('font-weight', 'bold')
      .style('fill', '#333')
      .text('Step Dependencies');
  }
});