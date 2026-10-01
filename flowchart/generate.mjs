#!/usr/bin/env node
// Regenerates diagrams/*.dot, diagrams/*.svg, index.html and
// source-manifest.json from flowchart-data.json.
//
// Rendering uses the Graphviz `dot` executable, or the WebAssembly build from
// @hpcc-js/wasm-graphviz when GRAPHVIZ_MODULE names its dist/index.js.
// References written as "path:function" are resolved to the line of that
// function's definition; generation fails when a reference no longer exists.

import { createHash } from 'node:crypto';
import { execFileSync, spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, '..');
const data = JSON.parse(readFileSync(join(here, 'flowchart-data.json'), 'utf8'));

const kindStyles = {
  external: 'shape=cylinder,style="filled",fillcolor="#eee8ff",color="#7253a6"',
  io: 'shape=parallelogram,style="filled",fillcolor="#e9f7ff",color="#24749c"',
  process: 'shape=box,style="rounded,filled",fillcolor="#e9f2ff",color="#3676b9"',
  end: 'shape=box,style="rounded,filled,bold",fillcolor="#dcfce7",color="#15803d"',
  start: 'shape=oval,style="filled",fillcolor="#d9f7f1",color="#17806d"',
  decision: 'shape=diamond,style="filled",fillcolor="#fff0cc",color="#b7791f"',
  error: 'shape=box,style="rounded,filled",fillcolor="#ffe3e0",color="#c2413b"',
  note: 'shape=note,style="filled",fillcolor="#f4f4f5",color="#71717a"'
};

const sources = new Map();

function source(path) {
  if (!sources.has(path)) {
    sources.set(path, readFileSync(join(repo, path), 'utf8').split('\n'));
  }
  return sources.get(path);
}

function escapeRegExp(text) {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// "src/x.c:name" -> "src/x.c:name:LINE"; a bare path refers to line 1.
function resolveRef(ref) {
  const [path, name] = ref.split(':');
  const lines = source(path);

  if (name === undefined) {
    return { path, text: `${path}:1` };
  }
  const definition = new RegExp(`^(?:[A-Za-z_][^;]*[\\s*])?${escapeRegExp(name)}\\s*\\(`);
  const index = lines.findIndex(
    line => definition.test(line) && !line.trimEnd().endsWith(';')
  );
  if (index < 0) {
    throw new Error(`unresolved flowchart reference ${ref}`);
  }
  return { path, text: `${path}:${name}:${index + 1}` };
}

function dotString(text) {
  return `"${text.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\n/g, '\\n')}"`;
}

function toDot(chart) {
  const lines = [
    'digraph G {',
    `  graph [rankdir=TB,bgcolor="white",pad="0.30",nodesep="0.35",ranksep="0.48",splines=polyline,fontname="Arial",label=${dotString(chart.title)},labelloc=t,fontsize=22,fontcolor="#0f172a"];`,
    '  node [fontname="Arial",fontsize=11,margin="0.14,0.09",penwidth=1.2];',
    '  edge [fontname="Arial",fontsize=9,color="#475569",fontcolor="#334155",arrowsize=0.7,penwidth=1.1];'
  ];

  for (const node of chart.nodes) {
    if (!kindStyles[node.kind]) {
      throw new Error(`${chart.id}: unknown node kind ${node.kind}`);
    }
    lines.push(`  ${node.id} [label=${dotString(node.label)},${kindStyles[node.kind]}];`);
  }
  const ids = new Set(chart.nodes.map(node => node.id));
  for (const edge of chart.edges) {
    if (!ids.has(edge.from) || !ids.has(edge.to)) {
      throw new Error(`${chart.id}: edge ${edge.from} -> ${edge.to} names an unknown node`);
    }
    const attributes = [];
    if (edge.label) {
      attributes.push(`label=${dotString(edge.label)}`);
    }
    if (edge.style === 'dashed') {
      attributes.push('style=dashed,color="#64748b"');
    }
    lines.push(`  ${edge.from} -> ${edge.to}${attributes.length ? ` [${attributes.join(',')}]` : ''};`);
  }
  lines.push('}', '');
  return lines.join('\n');
}

async function renderer() {
  if (process.env.GRAPHVIZ_MODULE) {
    const { Graphviz } = await import(pathToFileURL(resolve(process.env.GRAPHVIZ_MODULE)).href);
    const graphviz = await Graphviz.load();
    return { version: `${graphviz.version()} (@hpcc-js/wasm-graphviz)`, render: dot => graphviz.dot(dot) };
  }
  // dot -V prints its version on standard error.
  const version = spawnSync('dot', ['-V'], { encoding: 'utf8' });
  if (version.status !== 0) {
    throw new Error('Graphviz dot is not installed; set GRAPHVIZ_MODULE instead');
  }
  return {
    version: version.stderr.trim().replace(/^dot - graphviz version /, ''),
    render: dot => execFileSync('dot', ['-Tsvg'], { input: dot, encoding: 'utf8' })
  };
}

function html(text) {
  return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function git(...args) {
  return execFileSync('git', ['-C', repo, ...args], { encoding: 'utf8' }).trim();
}

const graphviz = await renderer();
const commit = git('rev-parse', '--short', 'HEAD');
const makefile = readFileSync(join(repo, 'Makefile'), 'utf8');
const packageVersion = `${makefile.match(/^PKG_VERSION:=(.*)$/m)[1]}-r${makefile.match(/^PKG_RELEASE:=(.*)$/m)[1]}`;
const referencedFiles = new Set();
const articles = [];

mkdirSync(join(here, 'diagrams'), { recursive: true });
data.charts.forEach((chart, index) => {
  const refs = chart.refs.map(resolveRef);
  const dot = toDot(chart);

  refs.forEach(ref => referencedFiles.add(ref.path));
  writeFileSync(join(here, 'diagrams', `${chart.id}.dot`), dot);
  writeFileSync(join(here, 'diagrams', `${chart.id}.svg`), graphviz.render(dot));
  articles.push(
    `<article class="chart" id="${chart.id}" data-index="${index}"><header><h2>${html(chart.title)}</h2>` +
    `<div class="refs">${refs.map(ref => `<code>${html(ref.text)}</code>`).join(' ')}</div></header>` +
    '<div class="toolbar"><button onclick="zoom(this,1.15)">Zoom +</button><button onclick="zoom(this,1/1.15)">Zoom −</button>' +
    `<button onclick="fit(this)">Fit width</button><a href="diagrams/${chart.id}.svg" target="_blank">Open SVG</a>` +
    `<a href="diagrams/${chart.id}.dot" download>DOT source</a><button onclick="savePng(this,'${chart.id}')">Save PNG</button></div>` +
    `<div class="canvas" data-scale="1"><img src="diagrams/${chart.id}.svg" alt="${html(chart.title)}"></div>` +
    `<details open><summary>Important behavior and caveats</summary><ul>${chart.notes.map(note => `<li>${html(note)}</li>`).join('')}</ul></details></article>`
  );
});

const style = `:root{color-scheme:light;--ink:#102033;--muted:#526173;--line:#d7e0ea;--paper:#f5f8fb}*{box-sizing:border-box}body{margin:0;font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;color:var(--ink);background:var(--paper)}.top{padding:30px max(24px,5vw);background:linear-gradient(135deg,#082f49,#155e75);color:white}.top h1{margin:0 0 8px;font-size:clamp(25px,4vw,42px)}.top p{max-width:1000px;margin:5px 0;color:#d8f3fb}.layout{display:grid;grid-template-columns:270px minmax(0,1fr);gap:22px;max-width:1800px;margin:auto;padding:22px}.nav{position:sticky;top:12px;align-self:start;max-height:calc(100vh - 24px);overflow:auto;background:white;border:1px solid var(--line);border-radius:12px;padding:12px}.nav a{display:block;padding:7px 9px;color:#164e63;text-decoration:none;border-radius:6px}.nav a:hover{background:#e6f5fa}.chart{background:white;border:1px solid var(--line);border-radius:14px;margin-bottom:24px;box-shadow:0 3px 14px #0f294018;overflow:hidden}.chart header{padding:18px 20px 10px}.chart h2{margin:0 0 8px}.refs{display:flex;gap:6px;flex-wrap:wrap}.refs code{font-size:12px;background:#edf2f7;padding:2px 6px;border-radius:5px}.toolbar{padding:9px 20px;background:#f3f7fa;border-block:1px solid var(--line);display:flex;gap:8px;flex-wrap:wrap}.toolbar button,.toolbar a{font:inherit;font-size:13px;padding:6px 10px;border:1px solid #abc0d2;border-radius:6px;background:white;color:#164e63;text-decoration:none;cursor:pointer}.canvas{overflow:auto;padding:18px;text-align:center;max-height:82vh}.canvas img{display:block;margin:auto;max-width:none;transform-origin:top left}.canvas[data-fit="yes"] img{width:100%;height:auto}.chart details{padding:0 20px 18px}.chart summary{font-weight:650;cursor:pointer}.chart li{margin:5px 0}.warning{margin-top:15px;padding:10px 13px;border-left:4px solid #f59e0b;background:#fffbeb;color:#78350f}.meta{font-size:13px}footer{padding:20px max(24px,5vw);color:var(--muted)}@media(max-width:900px){.layout{display:block}.nav{position:relative;max-height:none;margin-bottom:18px}.canvas{max-height:none}}`;
const script = `function box(button){return button.closest('.chart').querySelector('.canvas')}function zoom(button,f){const c=box(button);c.dataset.fit='no';const s=Math.max(.25,Math.min(4,Number(c.dataset.scale||1)*f));c.dataset.scale=s;c.querySelector('img').style.width=(s*100)+'%'}function fit(button){const c=box(button);c.dataset.fit='yes';c.dataset.scale=1;c.querySelector('img').style.width='100%'}async function savePng(button,id){const text=await fetch('diagrams/'+id+'.svg').then(r=>r.text());const blob=new Blob([text],{type:'image/svg+xml'});const url=URL.createObjectURL(blob);const image=new Image();image.onload=()=>{const vb=image.naturalWidth&&image.naturalHeight?[image.naturalWidth,image.naturalHeight]:[1600,1200];const scale=Math.min(2,16000/Math.max(...vb));const canvas=document.createElement('canvas');canvas.width=Math.round(vb[0]*scale);canvas.height=Math.round(vb[1]*scale);const ctx=canvas.getContext('2d');ctx.fillStyle='white';ctx.fillRect(0,0,canvas.width,canvas.height);ctx.drawImage(image,0,0,canvas.width,canvas.height);canvas.toBlob(p=>{const a=document.createElement('a');a.href=URL.createObjectURL(p);a.download=id+'.png';a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000)},'image/png');URL.revokeObjectURL(url)};image.src=url}`;

writeFileSync(
  join(here, 'index.html'),
  '<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">' +
  `<title>${html(data.title)}</title><style>\n${style}\n</style></head><body>` +
  `<section class="top"><h1>${html(data.title)}</h1><p>${html(data.description)}</p>` +
  `<p class="meta">Sources at commit <strong>${commit}</strong> (package ${html(packageVersion)}) · ${data.charts.length} vector diagrams · Graphviz ${html(graphviz.version)} · fully offline</p>` +
  `<div class="warning">${html(data.boundary)}</div></section>` +
  `<div class="layout"><nav class="nav"><strong>Diagrams</strong>${data.charts.map(chart => `<a href="#${chart.id}">${html(chart.title)}</a>`).join('')}</nav>` +
  `<main>${articles.join('\n')}</main></div>` +
  '<footer>Source references and SHA-256 hashes are in <code>source-manifest.json</code>. Edit <code>flowchart-data.json</code> and run <code>generate.mjs</code> to regenerate everything here.</footer>' +
  `<script>\n${script}\n</script></body></html>\n`
);

const sourceSha256 = {};
[...referencedFiles].sort().forEach(path => {
  sourceSha256[path] = createHash('sha256').update(readFileSync(join(repo, path))).digest('hex');
});
writeFileSync(
  join(here, 'source-manifest.json'),
  `${JSON.stringify({
    title: data.title,
    sourceCommit: commit,
    packageVersion,
    graphviz: graphviz.version,
    chartCount: data.charts.length,
    sourceSha256
  }, null, 2)}\n`
);
console.log(`generated ${data.charts.length} charts from ${commit} with Graphviz ${graphviz.version}`);
