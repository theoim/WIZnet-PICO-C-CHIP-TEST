#!/usr/bin/env node
'use strict';

const fs = require('fs');

const inputPath = process.argv[2];
const outputPath = process.argv[3];

if (!inputPath || !outputPath) {
  process.stderr.write('Usage: node ua-tour-analyze.js <input.json> <output.json>\n');
  process.exit(1);
}

let data;
try {
  data = JSON.parse(fs.readFileSync(inputPath, 'utf8'));
} catch (e) {
  process.stderr.write('Failed to parse input JSON: ' + e.message + '\n');
  process.exit(1);
}

const { nodes, edges, layers } = data;

// Build node map
const nodeMap = {};
for (const n of nodes) {
  nodeMap[n.id] = n;
}

// A. Fan-In & Fan-Out
const fanIn = {};
const fanOut = {};
for (const n of nodes) {
  fanIn[n.id] = 0;
  fanOut[n.id] = 0;
}
for (const e of edges) {
  if (fanOut[e.source] !== undefined) fanOut[e.source]++;
  if (fanIn[e.target] !== undefined) fanIn[e.target]++;
}

const fanInRanking = Object.entries(fanIn)
  .sort((a, b) => b[1] - a[1])
  .slice(0, 20)
  .map(([id, count]) => ({ id, fanIn: count, name: nodeMap[id] ? nodeMap[id].name : id }));

const fanOutRanking = Object.entries(fanOut)
  .sort((a, b) => b[1] - a[1])
  .slice(0, 20)
  .map(([id, count]) => ({ id, fanOut: count, name: nodeMap[id] ? nodeMap[id].name : id }));

// C. Entry Point Candidates
const entryFileNames = [
  'index.ts','index.js','main.ts','main.js','app.ts','app.js',
  'server.ts','server.js','mod.rs','main.go','main.py','main.rs',
  'manage.py','app.py','wsgi.py','asgi.py','run.py','__main__.py',
  'Application.java','Main.java','Program.cs','config.ru','index.php',
  'App.swift','Application.kt','main.cpp','main.c'
];

const totalNodes = nodes.length;
const fanOutValues = Object.values(fanOut).sort((a, b) => a - b);
const top10PctThreshold = fanOutValues[Math.floor(fanOutValues.length * 0.9)] || 0;
const bottom25PctThreshold = fanOutValues[Math.floor(fanOutValues.length * 0.25)] || 0;

const scores = {};
for (const n of nodes) {
  let score = 0;
  const fp = n.filePath || '';
  const parts = fp.split('/').filter(Boolean);

  if (n.type === 'document' && (n.name === 'README.md' && parts.length === 1)) {
    score += 5;
  } else if (n.type === 'document' && n.name.endsWith('.md') && parts.length === 1) {
    score += 2;
  } else if (n.type === 'file' || n.type === 'config') {
    if (entryFileNames.includes(n.name)) score += 3;
    if (parts.length <= 2) score += 1;
    if ((fanOut[n.id] || 0) >= top10PctThreshold) score += 1;
    if ((fanIn[n.id] || 0) <= bottom25PctThreshold) score += 1;
  }
  scores[n.id] = score;
}

const entryPointCandidates = Object.entries(scores)
  .sort((a, b) => b[1] - a[1])
  .slice(0, 5)
  .map(([id, score]) => ({
    id,
    score,
    name: nodeMap[id] ? nodeMap[id].name : id,
    type: nodeMap[id] ? nodeMap[id].type : '',
    summary: nodeMap[id] ? (nodeMap[id].summary || '') : ''
  }));

// D. BFS from top code entry point
// Find top code entry point (skip documents)
const topCodeEntry = entryPointCandidates.find(c => {
  const n = nodeMap[c.id];
  return n && n.type !== 'document';
});

const bfsStart = topCodeEntry ? topCodeEntry.id : null;

// Build adjacency list for imports/calls
const adj = {};
for (const n of nodes) adj[n.id] = [];
for (const e of edges) {
  if ((e.type === 'imports' || e.type === 'calls') && adj[e.source]) {
    adj[e.source].push(e.target);
  }
}

const bfsOrder = [];
const depthMap = {};
const byDepth = {};

if (bfsStart) {
  const visited = new Set();
  const queue = [[bfsStart, 0]];
  visited.add(bfsStart);
  while (queue.length > 0) {
    const [nodeId, depth] = queue.shift();
    bfsOrder.push(nodeId);
    depthMap[nodeId] = depth;
    if (!byDepth[depth]) byDepth[depth] = [];
    byDepth[depth].push(nodeId);
    for (const neighbor of (adj[nodeId] || [])) {
      if (!visited.has(neighbor)) {
        visited.add(neighbor);
        queue.push([neighbor, depth + 1]);
      }
    }
  }
}

const bfsTraversal = {
  startNode: bfsStart,
  order: bfsOrder,
  depthMap,
  byDepth
};

// E. Non-Code File Inventory
const nonCodeFiles = { documentation: [], infrastructure: [], data: [], config: [] };
for (const n of nodes) {
  if (n.type === 'document') {
    nonCodeFiles.documentation.push({ id: n.id, name: n.name, summary: n.summary || '' });
  } else if (['service','pipeline','resource'].includes(n.type)) {
    nonCodeFiles.infrastructure.push({ id: n.id, name: n.name, type: n.type, summary: n.summary || '' });
  } else if (['table','schema','endpoint'].includes(n.type)) {
    nonCodeFiles.data.push({ id: n.id, name: n.name, type: n.type, summary: n.summary || '' });
  } else if (n.type === 'config') {
    nonCodeFiles.config.push({ id: n.id, name: n.name, summary: n.summary || '' });
  }
}

// F. Tightly Coupled Clusters
const edgeSet = new Set();
const bidirectional = [];
for (const e of edges) {
  const key = e.source + '|||' + e.target;
  edgeSet.add(key);
}
const pairsSeen = new Set();
for (const e of edges) {
  const reverseKey = e.target + '|||' + e.source;
  const pairKey = [e.source, e.target].sort().join('|||');
  if (edgeSet.has(reverseKey) && !pairsSeen.has(pairKey)) {
    pairsSeen.add(pairKey);
    bidirectional.push([e.source, e.target]);
  }
}

// Build clusters from bidirectional pairs, then expand
const clusterMap = {};
for (const [a, b] of bidirectional) {
  let found = null;
  for (const key of Object.keys(clusterMap)) {
    if (clusterMap[key].has(a) || clusterMap[key].has(b)) {
      found = key;
      break;
    }
  }
  if (found) {
    clusterMap[found].add(a);
    clusterMap[found].add(b);
  } else {
    const key = a + '|||' + b;
    clusterMap[key] = new Set([a, b]);
  }
}

// Also group nodes that share the same header (high co-import pattern)
// Group impl files that import the same header
const headerImporters = {};
for (const e of edges) {
  if (e.type === 'imports') {
    if (!headerImporters[e.target]) headerImporters[e.target] = [];
    headerImporters[e.target].push(e.source);
  }
}

const implClusters = [];
for (const [header, importers] of Object.entries(headerImporters)) {
  if (importers.length >= 2 && importers.length <= 5) {
    implClusters.push({ nodes: [header, ...importers], edgeCount: importers.length });
  }
}

// Combine clusters
const allClusters = [
  ...Object.values(clusterMap).map(s => ({ nodes: Array.from(s), edgeCount: s.size })),
  ...implClusters
].sort((a, b) => b.edgeCount - a.edgeCount).slice(0, 10);

// G. Layers
const layerInfo = {
  count: layers.length,
  list: layers.map(l => ({ id: l.id, name: l.name, description: l.description }))
};

// H. Node Summary Index
const nodeSummaryIndex = {};
for (const n of nodes) {
  nodeSummaryIndex[n.id] = { name: n.name, type: n.type, summary: n.summary || '' };
}

const result = {
  scriptCompleted: true,
  entryPointCandidates,
  fanInRanking,
  fanOutRanking,
  bfsTraversal,
  nonCodeFiles,
  clusters: allClusters,
  layers: layerInfo,
  nodeSummaryIndex,
  totalNodes: nodes.length,
  totalEdges: edges.length
};

try {
  fs.writeFileSync(outputPath, JSON.stringify(result, null, 2), 'utf8');
  process.stdout.write('Analysis complete. Results written to ' + outputPath + '\n');
  process.exit(0);
} catch (e) {
  process.stderr.write('Failed to write output: ' + e.message + '\n');
  process.exit(1);
}
