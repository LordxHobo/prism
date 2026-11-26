// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Prism's hidden translation page (Translate.cpp opens it, served by Prism's
// local server at http://prism-translate.localhost:<port>/). It runs the
// language detector (CLD2) and the translator (translator-worker.js) in
// workers. Prism calls prismEngine.handle(id, request); answers go back
// through window.prompt("prism-translate:<id>\n<JSON>"), which only Prism
// sees.

'use strict';

(function () {
  function answer(id, value) {
    try {
      window.prompt('prism-translate:' + id + '\n' + JSON.stringify(value));
    } catch (e) { /* Prism closed the page */ }
  }

  // ---- Language detection (CLD2) ----------------------------------------------------

  let detector = null;
  let detectorReady = null;
  const detectWaiting = []; // FIFO: CLD answers in order

  function startDetector() {
    if (detectorReady) return detectorReady;
    detectorReady = new Promise((resolve, reject) => {
      detector = new Worker('cld-worker.js');
      detector.onmessage = (event) => {
        if (event.data === 'ready') {
          resolve();
          return;
        }
        const next = detectWaiting.shift();
        if (next) next.resolve(event.data);
      };
      detector.onerror = (event) => {
        event.preventDefault();
        const error = new Error('The language detector stopped.');
        while (detectWaiting.length) detectWaiting.shift().reject(error);
        detector = null;
        detectorReady = null;
        reject(error);
      };
    });
    return detectorReady;
  }

  async function detect(text) {
    await startDetector();
    return new Promise((resolve, reject) => {
      detectWaiting.push({ resolve, reject });
      detector.postMessage({ text: String(text || '').slice(0, 8000), isHTML: false });
    });
  }

  // ---- Translation ------------------------------------------------------------------

  let translator = null;
  const translateWaiting = new Map(); // id -> { resolve, reject }
  let nextJob = 1;

  function startTranslator() {
    if (translator) return translator;
    translator = new Worker('translator-worker.js');
    translator.onmessage = (event) => {
      const data = event.data || {};
      const waiting = translateWaiting.get(data.id);
      if (!waiting) return;
      translateWaiting.delete(data.id);
      if (data.error) waiting.reject(new Error(data.error));
      else waiting.resolve(data.results);
    };
    translator.onerror = (event) => {
      event.preventDefault();
      const error = new Error('The translator stopped (it may have run out of memory).');
      for (const waiting of translateWaiting.values()) waiting.reject(error);
      translateWaiting.clear();
      try { translator.terminate(); } catch (e) { /* gone */ }
      translator = null;
    };
    return translator;
  }

  function translate(request) {
    const worker = startTranslator();
    const job = nextJob++;
    return new Promise((resolve, reject) => {
      translateWaiting.set(job, { resolve, reject });
      worker.postMessage({ id: job, op: 'translate', wasm: request.wasm, models: request.models, items: request.items });
    });
  }

  window.prismEngine = Object.freeze({
    handle(id, request) {
      let work;
      if (request.op === 'detect') {
        work = detect(request.text).then((result) => ({ ok: true, result }));
      } else if (request.op === 'translate') {
        work = translate(request).then((results) => ({ ok: true, results }));
      } else if (request.op === 'unload') {
        if (translator) {
          translator.terminate();
          translator = null;
        }
        work = Promise.resolve({ ok: true });
      } else {
        work = Promise.resolve({ ok: false, error: 'Unknown request' });
      }
      work.then((value) => answer(id, value), (error) => answer(id, { ok: false, error: String((error && error.message) || error) }));
      return 'started';
    },
  });

  answer(0, { ready: true });
})();
