#!/usr/bin/env node
'use strict';

const fs = require('fs');

const inputPath = process.argv[2];
const outputPath = process.argv[3];

if (!inputPath || !outputPath) {
  console.error('Usage: node ua-arch-analyze.js <input.json> <output.json>');
  process.exit(1);
}

let input;
try {
  input = JSON.parse(fs.readFileSync(inputPath, 'utf8'));
} catch (e) {
  console.error('Failed to read/parse input:', e.message);
  process.exit(1);
}

const { fileNodes, importEdges, allEdges } = input;

// ── A. Directory Grouping ──────────────────────────────────────────────────
function getCommonPrefix(paths) {
  if (!paths.length) return '';
  const parts = paths.map(p => p.replace(/\\/g, '/').split('/'));
  const minLen = Math.min(...parts.map(p => p.length));
  let prefix = [];
  for (let i = 0; i < minLen - 1; i++) {
    const seg = parts[0][i];
    if (parts.every(p => p[i] === seg)) prefix.push(seg);
    else break;
  }
  return prefix.length ? prefix.join('/') + '/' : '';
}

const allPaths = fileNodes.map(n => n.filePath.replace(/\\/g, '/'));
const commonPrefix = getCommonPrefix(allPaths);

function getGroupKey(filePath) {
  const fp = filePath.replace(/\\/g, '/');
  const stripped = commonPrefix ? fp.replace(commonPrefix, '') : fp;
  const parts = stripped.split('/');
  if (parts.length === 1) return 'root';
  return parts[0];
}

const directoryGroups = {};
for (const node of fileNodes) {
  const key = getGroupKey(node.filePath);
  if (!directoryGroups[key]) directoryGroups[key] = [];
  directoryGroups[key].push(node.id);
}

// ── B. Node Type Grouping ──────────────────────────────────────────────────
const nodeTypeGroups = {};
for (const node of fileNodes) {
  const t = node.type || 'file';
  if (!nodeTypeGroups[t]) nodeTypeGroups[t] = [];
  nodeTypeGroups[t].push(node.id);
}

// ── C. Import Adjacency / Fan-in / Fan-out ─────────────────────────────────
const fanIn = {};
const fanOut = {};
for (const node of fileNodes) {
  fanIn[node.id] = 0;
  fanOut[node.id] = 0;
}
for (const edge of importEdges) {
  if (edge.type === 'imports') {
    fanOut[edge.source] = (fanOut[edge.source] || 0) + 1;
    fanIn[edge.target] = (fanIn[edge.target] || 0) + 1;
  }
}

// ── D. Cross-Category Dependency Analysis ─────────────────────────────────
const nodeTypeMap = {};
for (const node of fileNodes) nodeTypeMap[node.id] = node.type || 'file';

const crossCategoryMap = {};
for (const edge of allEdges) {
  const fromType = nodeTypeMap[edge.source] || 'unknown';
  const toType = nodeTypeMap[edge.target] || 'unknown';
  if (fromType !== toType || fromType !== 'file') {
    const key = `${fromType}||${toType}||${edge.type}`;
    crossCategoryMap[key] = (crossCategoryMap[key] || 0) + 1;
  }
}
const crossCategoryEdges = Object.entries(crossCategoryMap).map(([k, count]) => {
  const [fromType, toType, edgeType] = k.split('||');
  return { fromType, toType, edgeType, count };
});

// ── E. Inter-Group Import Frequency ───────────────────────────────────────
const nodeGroupMap = {};
for (const [grp, ids] of Object.entries(directoryGroups)) {
  for (const id of ids) nodeGroupMap[id] = grp;
}

const interGroupMap = {};
for (const edge of importEdges) {
  if (edge.type !== 'imports') continue;
  const from = nodeGroupMap[edge.source];
  const to = nodeGroupMap[edge.target];
  if (!from || !to || from === to) continue;
  const key = `${from}||${to}`;
  interGroupMap[key] = (interGroupMap[key] || 0) + 1;
}
const interGroupImports = Object.entries(interGroupMap).map(([k, count]) => {
  const [from, to] = k.split('||');
  return { from, to, count };
}).sort((a, b) => b.count - a.count);

// ── F. Intra-Group Import Density ─────────────────────────────────────────
const intraGroupCounts = {};
const totalGroupEdgeCounts = {};
for (const grp of Object.keys(directoryGroups)) {
  intraGroupCounts[grp] = 0;
  totalGroupEdgeCounts[grp] = 0;
}
for (const edge of importEdges) {
  if (edge.type !== 'imports') continue;
  const from = nodeGroupMap[edge.source];
  const to = nodeGroupMap[edge.target];
  if (from) totalGroupEdgeCounts[from] = (totalGroupEdgeCounts[from] || 0) + 1;
  if (to) totalGroupEdgeCounts[to] = (totalGroupEdgeCounts[to] || 0) + 1;
  if (from && to && from === to) intraGroupCounts[from] = (intraGroupCounts[from] || 0) + 1;
}
const intraGroupDensity = {};
for (const grp of Object.keys(directoryGroups)) {
  const internal = intraGroupCounts[grp] || 0;
  const total = totalGroupEdgeCounts[grp] || 0;
  intraGroupDensity[grp] = {
    internalEdges: internal,
    totalEdges: total,
    density: total > 0 ? parseFloat((internal / total).toFixed(3)) : 0
  };
}

// ── G. Directory Pattern Matching ─────────────────────────────────────────
const DIR_PATTERNS = {
  routes: 'api', api: 'api', controllers: 'api', endpoints: 'api', handlers: 'api',
  serializers: 'api', blueprints: 'api', routers: 'api', controller: 'api',
  services: 'service', core: 'service', lib: 'service', domain: 'service', logic: 'service',
  composables: 'service', internal: 'service', signals: 'service', mailers: 'service',
  jobs: 'service', channels: 'service',
  models: 'data', db: 'data', data: 'data', persistence: 'data', repository: 'data',
  entities: 'data', migrations: 'data', entity: 'data', sql: 'data', database: 'data',
  schema: 'data',
  components: 'ui', views: 'ui', pages: 'ui', ui: 'ui', layouts: 'ui', screens: 'ui',
  middleware: 'middleware', plugins: 'middleware', interceptors: 'middleware', guards: 'middleware',
  utils: 'utility', helpers: 'utility', common: 'utility', shared: 'utility', tools: 'utility',
  pkg: 'utility', templatetags: 'utility',
  config: 'config', constants: 'config', env: 'config', settings: 'config',
  management: 'config', commands: 'config',
  '__tests__': 'test', test: 'test', tests: 'test', spec: 'test', specs: 'test',
  types: 'types', interfaces: 'types', schemas: 'types', contracts: 'types', dtos: 'types',
  dto: 'types', request: 'types', response: 'types',
  hooks: 'hooks',
  store: 'state', state: 'state', reducers: 'state', actions: 'state', slices: 'state',
  assets: 'assets', static: 'assets', public: 'assets',
  cmd: 'entry', bin: 'entry',
  docs: 'documentation', documentation: 'documentation', wiki: 'documentation',
  deploy: 'infrastructure', deployment: 'infrastructure', infra: 'infrastructure',
  infrastructure: 'infrastructure', k8s: 'infrastructure', kubernetes: 'infrastructure',
  helm: 'infrastructure', charts: 'infrastructure', terraform: 'infrastructure',
  tf: 'infrastructure', docker: 'infrastructure',
  '.github': 'ci-cd', '.gitlab': 'ci-cd', '.circleci': 'ci-cd',
  // project-specific
  examples: 'entry',
  port: 'service',
  libraries: 'utility',
};

const patternMatches = {};
for (const grp of Object.keys(directoryGroups)) {
  patternMatches[grp] = DIR_PATTERNS[grp.toLowerCase()] || 'unknown';
}

// ── H. Deployment Topology ────────────────────────────────────────────────
const infraPatterns = [/dockerfile/i, /docker-compose/i, /\.tf$/, /\.tfvars$/, /makefile/i];
const ciPatterns = [/\.github\/workflows/i, /\.gitlab-ci/i, /jenkinsfile/i, /\.circleci/i];
const k8sPatterns = [/k8s\//i, /kubernetes\//i, /helm\//i];
const terraformPatterns = [/\.tf$/, /terraform\//i];

const infraFiles = [];
let hasDockerfile = false, hasCompose = false, hasK8s = false, hasTerraform = false, hasCI = false;

for (const node of fileNodes) {
  const fp = node.filePath.replace(/\\/g, '/').toLowerCase();
  if (/dockerfile/.test(fp)) { hasDockerfile = true; infraFiles.push(node.filePath); }
  if (/docker-compose/.test(fp)) { hasCompose = true; infraFiles.push(node.filePath); }
  if (k8sPatterns.some(p => p.test(fp))) { hasK8s = true; infraFiles.push(node.filePath); }
  if (terraformPatterns.some(p => p.test(fp))) { hasTerraform = true; infraFiles.push(node.filePath); }
  if (ciPatterns.some(p => p.test(fp))) { hasCI = true; infraFiles.push(node.filePath); }
}

const deploymentTopology = {
  hasDockerfile, hasCompose, hasK8s, hasTerraform, hasCI,
  infraFiles: [...new Set(infraFiles)]
};

// ── I. Data Pipeline Detection ────────────────────────────────────────────
const schemaFiles = fileNodes.filter(n => /\.(graphql|gql|proto|prisma|sql)$/.test(n.filePath)).map(n => n.filePath);
const migrationFiles = fileNodes.filter(n => /migrations?\//i.test(n.filePath)).map(n => n.filePath);
const dataModelFiles = fileNodes.filter(n => /model|entity|table|schema/i.test(n.name)).map(n => n.filePath);
const apiHandlerFiles = fileNodes.filter(n => /route|controller|handler|endpoint/i.test(n.name)).map(n => n.filePath);

const dataPipeline = { schemaFiles, migrationFiles, dataModelFiles, apiHandlerFiles };

// ── J. Documentation Coverage ─────────────────────────────────────────────
const docNodes = fileNodes.filter(n => n.type === 'document' || /\.(md|rst)$/.test(n.filePath));
const groupsWithDocs = new Set();
for (const doc of docNodes) {
  const grp = getGroupKey(doc.filePath);
  groupsWithDocs.add(grp);
}
const totalGroups = Object.keys(directoryGroups).length;
const undocumentedGroups = Object.keys(directoryGroups).filter(g => !groupsWithDocs.has(g));
const docCoverage = {
  groupsWithDocs: groupsWithDocs.size,
  totalGroups,
  coverageRatio: parseFloat((groupsWithDocs.size / totalGroups).toFixed(2)),
  undocumentedGroups
};

// ── K. Dependency Direction ───────────────────────────────────────────────
const pairImports = {};
for (const edge of importEdges) {
  if (edge.type !== 'imports') continue;
  const from = nodeGroupMap[edge.source];
  const to = nodeGroupMap[edge.target];
  if (!from || !to || from === to) continue;
  const fwd = `${from}||${to}`;
  const rev = `${to}||${from}`;
  pairImports[fwd] = (pairImports[fwd] || 0) + 1;
  pairImports[rev] = pairImports[rev] || 0;
}
const seen = new Set();
const dependencyDirection = [];
for (const [key, count] of Object.entries(pairImports)) {
  const [a, b] = key.split('||');
  const revKey = `${b}||${a}`;
  const pairId = [a, b].sort().join('||');
  if (seen.has(pairId)) continue;
  seen.add(pairId);
  const fwd = pairImports[`${a}||${b}`] || 0;
  const rev = pairImports[`${b}||${a}`] || 0;
  if (fwd > rev) dependencyDirection.push({ dependent: a, dependsOn: b });
  else if (rev > fwd) dependencyDirection.push({ dependent: b, dependsOn: a });
}

// ── File Stats ────────────────────────────────────────────────────────────
const filesPerGroup = {};
for (const [grp, ids] of Object.entries(directoryGroups)) filesPerGroup[grp] = ids.length;
const nodeTypeCounts = {};
for (const [t, ids] of Object.entries(nodeTypeGroups)) nodeTypeCounts[t] = ids.length;

const fileStats = {
  totalFileNodes: fileNodes.length,
  filesPerGroup,
  nodeTypeCounts
};

// ── Output ────────────────────────────────────────────────────────────────
const result = {
  scriptCompleted: true,
  directoryGroups,
  nodeTypeGroups,
  crossCategoryEdges,
  interGroupImports,
  intraGroupDensity,
  patternMatches,
  deploymentTopology,
  dataPipeline,
  docCoverage,
  dependencyDirection,
  fileStats,
  fileFanIn: fanIn,
  fileFanOut: fanOut
};

try {
  fs.writeFileSync(outputPath, JSON.stringify(result, null, 2), 'utf8');
  console.log('Analysis complete. Total nodes:', fileNodes.length);
  process.exit(0);
} catch (e) {
  console.error('Failed to write output:', e.message);
  process.exit(1);
}
