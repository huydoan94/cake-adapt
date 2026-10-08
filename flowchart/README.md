# cake-adapt current-code flowcharts

[View the rendered flowchart viewer](https://raw.githack.com/huydoan94/cake-adapt/further-integration/flowchart/index.html),
or open `index.html` locally. The viewer contains 16 linked diagrams and works
without a build step. The hosted link reads the `main` branch, so it shows the
latest version pushed to GitHub.

- Structured graph data, the only file edited by hand: `flowchart-data.json`
- Generator: `generate.mjs`
- Editable Graphviz sources and standalone vector images: `diagrams/*.dot`, `diagrams/*.svg`
- Source commit, package version, Graphviz version and SHA-256 of every
  referenced source file: `source-manifest.json`

The diagrams describe the sources listed in `source-manifest.json`. A source
file whose current hash differs from the manifest may have changed since the
charts were drawn.

## Regenerating

Edit `flowchart-data.json`, then run the generator with Node.js 18 or newer:

```sh
node flowchart/generate.mjs
```

It uses the Graphviz `dot` executable. Without Graphviz installed, point
`GRAPHVIZ_MODULE` at the WebAssembly build from the
`@hpcc-js/wasm-graphviz` npm package instead:

```sh
GRAPHVIZ_MODULE=/path/to/node_modules/@hpcc-js/wasm-graphviz/dist/index.js \
    node flowchart/generate.mjs
```

The committed diagrams were rendered with `@hpcc-js/wasm-graphviz` 1.29.2
(Graphviz 16.1.0).

A chart reference is written as `path:function`. The generator resolves it to
the line of that function's definition, and fails if the function no longer
exists. Code changes therefore surface stale references at the next
regeneration.
