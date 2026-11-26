// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Prism's translator: Bergamot (the engine of Firefox Translations, compiled
// to WebAssembly) in a worker of the hidden translation page (engine.js).
// The engine and the language models are files Prism downloaded from
// Mozilla and checked (Translate.cpp); they're zstd-compressed, as Mozilla
// serves them. Model setup follows Firefox's translations-engine.worker.js.
//
// Messages: {id, op: "translate", wasm, models: [{key, from, to, files}],
// items: [{h: is HTML, t: text}]} -> {id, results: [text or null]}, or
// {id, error}. {id, op: "unload"} frees the models.

'use strict';

importScripts('bergamot-translator.js', 'fzstd.js');

const MODEL_FILE_ALIGNMENTS = { model: 256, lex: 64, vocab: 64, srcvocab: 64, trgvocab: 64 };
const MAX_MODELS = 4; // a few language pairs in memory at once
const WHITESPACE = /^(\s*)(.*?)(\s*)$/s;
const FULL_WIDTH_PUNCTUATION = /([。！？])“/g;
const FULL_WIDTH_LANGUAGES = ['ja', 'ko', 'zh'];

let bergamot = null;
let service = null;
const models = new Map(); // key -> { model, used }

async function fetchBytes(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error('A translation file is missing (' + response.status + ').');
  return new Uint8Array(await response.arrayBuffer());
}

// Mozilla's files are zstd frames; anything else is used as it is.
function decompress(bytes) {
  if (bytes.length >= 4 && bytes[0] === 0x28 && bytes[1] === 0xb5 && bytes[2] === 0x2f && bytes[3] === 0xfd)
    return fzstd.decompress(bytes);
  return bytes;
}

function ownBuffer(bytes) {
  return bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength ? bytes.buffer : bytes.slice().buffer;
}

async function startEngine(wasmURL) {
  if (bergamot) return;
  const wasm = decompress(await fetchBytes(wasmURL));
  bergamot = await new Promise((resolve, reject) => {
    const module = loadBergamot({
      // Firefox starts at 40 MiB and lets it grow.
      INITIAL_MEMORY: 41943040,
      print: () => {},
      printErr: (text) => console.warn('Bergamot:', text),
      onAbort() {
        reject(new Error('The translation engine couldn’t start.'));
      },
      onRuntimeInitialized: async () => {
        await Promise.resolve();
        resolve(module);
      },
      wasmBinary: ownBuffer(wasm),
    });
  });
  service = new bergamot.BlockingService({ cacheSize: 0 });
}

function configText(entries) {
  const indent = '            ';
  let text = '\n';
  for (const [key, value] of Object.entries(entries)) text += indent + key + ': ' + value + '\n';
  return text + indent;
}

async function loadModel(spec) {
  const cached = models.get(spec.key);
  if (cached) {
    cached.used = Date.now();
    return cached.model;
  }
  const memory = {};
  for (const [type, file] of Object.entries(spec.files)) {
    const alignment = MODEL_FILE_ALIGNMENTS[type];
    if (!alignment) continue;
    const bytes = decompress(await fetchBytes(file.url));
    const aligned = new bergamot.AlignedMemory(bytes.byteLength, alignment);
    aligned.getByteArrayView().set(bytes);
    memory[type] = aligned;
  }
  if (!memory.model) throw new Error('The language model is missing.');
  const vocabs = new bergamot.AlignedMemoryList();
  if (memory.vocab) {
    vocabs.push_back(memory.vocab);
  } else if (memory.srcvocab && memory.trgvocab) {
    vocabs.push_back(memory.srcvocab);
    vocabs.push_back(memory.trgvocab);
  } else {
    throw new Error('The language model has no vocabulary.');
  }
  const modelName = (spec.files.model && spec.files.model.name) || '';
  // The settings Firefox uses (quality scores off, one beam: fast and good).
  const config = configText({
    'beam-size': '1',
    normalize: '1.0',
    'word-penalty': '0',
    'max-length-break': '128',
    'mini-batch-words': '1024',
    workspace: '128',
    'max-length-factor': '2.0',
    'skip-cost': 'true',
    'cpu-threads': '0',
    quiet: 'true',
    'quiet-translation': 'true',
    'gemm-precision': /intgemm8\.bin(\.zst)?$/.test(modelName) ? 'int8shiftAll' : 'int8shiftAlphaAll',
    alignment: 'soft',
  });
  const model = new bergamot.TranslationModel(spec.from, spec.to, config, memory.model, memory.lex || null, vocabs, null);
  // Keep only the most recently used pairs.
  while (models.size >= MAX_MODELS) {
    let oldestKey = null;
    let oldest = Infinity;
    for (const [key, entry] of models) {
      if (entry.used < oldest) {
        oldest = entry.used;
        oldestKey = key;
      }
    }
    if (oldestKey === null) break;
    try { models.get(oldestKey).model.delete(); } catch (e) { /* already gone */ }
    models.delete(oldestKey);
  }
  models.set(spec.key, { model, used: Date.now() });
  return model;
}

function clean(language, text) {
  const match = WHITESPACE.exec(text);
  let middle = match[2].replaceAll('­', '');
  if (FULL_WIDTH_LANGUAGES.includes(language)) middle = middle.replaceAll(FULL_WIDTH_PUNCTUATION, '$1 “');
  return { before: match[1], text: middle, after: match[3] };
}

function runBatch(chain, from, items) {
  const messages = new bergamot.VectorString();
  const options = new bergamot.VectorResponseOptions();
  const results = items.map((item) => (typeof item.t === 'string' ? item.t : ''));
  const slots = [];
  let responses = null;
  try {
    items.forEach((item, index) => {
      const parts = clean(from, String(item.t || ''));
      if (!parts.text.trim()) return;
      messages.push_back(parts.text);
      options.push_back({ qualityScores: false, alignment: true, html: !!item.h });
      slots.push({ index, before: parts.before, after: parts.after });
    });
    if (!slots.length) return results;
    responses = chain.length === 1
      ? service.translate(chain[0], messages, options)
      : service.translateViaPivoting(chain[0], chain[1], messages, options);
    slots.forEach((slot, j) => {
      results[slot.index] = slot.before + responses.get(j).getTranslatedText() + slot.after;
    });
    return results;
  } finally {
    messages.delete();
    options.delete();
    if (responses) responses.delete();
  }
}

async function translate(message) {
  await startEngine(message.wasm);
  const chain = [];
  for (const spec of message.models) chain.push(await loadModel(spec));
  const from = message.models[0].from;
  const items = message.items || [];
  try {
    return runBatch(chain, from, items);
  } catch (error) {
    // One bad piece (like HTML the engine can't read) fails the whole
    // batch: try them one by one, and leave the ones that fail as they are.
    return items.map((item) => {
      try {
        return runBatch(chain, from, [item])[0];
      } catch (e) {
        return null;
      }
    });
  }
}

let queue = Promise.resolve();

self.onmessage = (event) => {
  const message = event.data || {};
  // One at a time: the engine isn't re-entrant.
  queue = queue.then(async () => {
    try {
      if (message.op === 'unload') {
        for (const entry of models.values()) {
          try { entry.model.delete(); } catch (e) { /* already gone */ }
        }
        models.clear();
        self.postMessage({ id: message.id, results: [] });
        return;
      }
      const results = await translate(message);
      self.postMessage({ id: message.id, results });
    } catch (error) {
      self.postMessage({ id: message.id, error: String((error && error.message) || error) });
    }
  });
};
