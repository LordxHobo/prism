// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Builds Prism Shields' scriptlet library from uBlock Origin's source.
//
// Filter lists use "scriptlets" (small scripts like json-prune or
// set-constant) to defuse ads that come from a site's own servers, such as
// YouTube's. uBlock Origin maintains the reference implementations as ES
// modules in src/js/resources/. This script bundles them into the resource
// format of adblock-rust (Brave's engine, which Prism Shields uses):
//
//   [{ "name": "json-prune.js", "aliases": [...], "kind": { "mime": "application/javascript" },
//      "content": base64(function source), "dependencies": [...], "permission": 0|1 }]
//
// Usage: node build-scriptlets.mjs <uBlock/src/js/resources> <output.json>
//
// The scriptlets are uBlock Origin's code (GPLv3). They are fetched and built
// at build time and shipped as a separate data file next to Prism, with
// uBlock Origin's license (see dist/shields/README.txt).

import fs from 'fs';
import path from 'path';
import vm from 'vm';

const [sourceDir, outputFile] = process.argv.slice(2);
if (!sourceDir || !outputFile) {
  console.error('Usage: node build-scriptlets.mjs <uBlock/src/js/resources> <output.json>');
  process.exit(1);
}

// Every module, in the order an ES module loader would run them: a module's
// imports first (scriptlets register their dependencies by reference, so
// those must be registered already). A few helpers live one folder up.
const sources = new Map();   // absolute path -> source
const ordered = [];          // absolute paths
const visiting = new Set();
const importRe = /^import\s+([\s\S]*?\s+from\s+)?['"](\.{1,2}\/[^'"]+)['"]\s*;?/gm;

function visit(file) {
  if (ordered.includes(file) || visiting.has(file)) return;
  if (!sources.has(file)) {
    if (!fs.existsSync(file)) throw new Error('Missing module ' + file);
    sources.set(file, fs.readFileSync(file, 'utf8'));
  }
  visiting.add(file);
  for (const match of sources.get(file).matchAll(importRe)) {
    const target = path.resolve(path.dirname(file), match[2]);
    visit(target);
  }
  visiting.delete(file);
  ordered.push(file);
}

const root = path.resolve(sourceDir);
visit(path.join(root, 'scriptlets.js'));
for (const name of fs.readdirSync(root).sort()) {
  if (name.endsWith('.js')) visit(path.join(root, name)); // anything not reachable from scriptlets.js
}

// A tiny ES module emulation: each module runs in its own function scope and
// returns its exports; imports read from modules that already ran.
const moduleId = (file) => JSON.stringify(path.relative(root, file));

function convertModule(file, source) {
  const exported = [];
  let code = source.replace(importRe, (whole, clause, specifier) => {
    const target = moduleId(path.resolve(path.dirname(file), specifier));
    const names = /\{([^}]*)\}/.exec(clause || '');
    if (!names) return '';
    const bindings = names[1].split(',').map((part) => part.trim()).filter(Boolean)
      .map((part) => part.replace(/^(\w+)\s+as\s+(\w+)$/, '$1: $2'));
    return `const { ${bindings.join(', ')} } = __modules[${target}];`;
  });
  code = code.replace(/^export\s+(?:async\s+)?function\s+(\w+)/gm, (whole, name) => {
    exported.push(name);
    return whole.replace(/^export\s+/, '');
  });
  code = code.replace(/^export\s+(?:const|let|var|class)\s+(\w+)/gm, (whole, name) => {
    exported.push(name);
    return whole.replace(/^export\s+/, '');
  });
  code = code.replace(/^export\s*\{([^}]*)\}\s*;?/gm, (whole, list) => {
    for (const part of list.split(',')) {
      const name = part.trim().split(/\s+as\s+/).pop();
      if (name) exported.push(name);
    }
    return '';
  });
  return `__modules[${moduleId(file)}] = (function () {\n${code}\nreturn { ${[...new Set(exported)].join(', ')} };\n})();\n`;
}

let bundle = 'const __modules = {};\n';
for (const file of ordered)
  bundle += `\n// ---- ${path.relative(root, file)}\n` + convertModule(file, sources.get(file));
bundle += `\nconst registeredScriptlets = __modules[${moduleId(path.join(root, 'base.js'))}].registeredScriptlets;\n`;
bundle += '\n;globalThis.__prismScriptlets = registeredScriptlets;\n';

// Evaluate in a sandbox: this only defines functions and fills the registry.
const sandbox = { console };
vm.createContext(sandbox);
vm.runInContext(bundle, sandbox, { filename: 'ublock-scriptlets-bundle.js' });
const registered = sandbox.__prismScriptlets;
if (!Array.isArray(registered) || registered.length < 20) {
  throw new Error(`Only found ${registered && registered.length} scriptlets; uBlock Origin's layout may have changed.`);
}

const resources = [];
const seen = new Set();
for (const details of registered) {
  if (!details || typeof details.name !== 'string' || typeof details.fn !== 'function') continue;
  if (seen.has(details.name)) continue;
  seen.add(details.name);
  const dependencies = (details.dependencies || []).map((dep) => (typeof dep === 'function' ? dep.details.name : dep));
  resources.push({
    name: details.name,
    aliases: details.aliases || [],
    // Injectable scriptlets ("*.js") must be application/javascript; helper
    // functions they depend on ("*.fn") are fn/javascript.
    kind: { mime: details.name.endsWith('.fn') ? 'fn/javascript' : 'application/javascript' },
    content: Buffer.from(details.fn.toString(), 'utf8').toString('base64'),
    dependencies,
    // Trusted scriptlets may only be used by uBlock Origin's own lists.
    permission: details.requiresTrust ? 1 : 0,
  });
}

fs.mkdirSync(path.dirname(outputFile), { recursive: true });
fs.writeFileSync(outputFile, JSON.stringify(resources));
console.log(`Wrote ${resources.length} scriptlets to ${outputFile}`);
