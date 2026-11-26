# Prism's translator

Pages are translated on this computer with Bergamot, the engine of Firefox
Translations. Nothing you read is sent anywhere.

- `engine.html`, `engine.js`: the hidden page that runs the translator.
- `translator-worker.js`: loads Bergamot and the language models, translates.
- `bergamot-translator.js`: Bergamot's JavaScript, from Firefox
  (`toolkit/components/translations/bergamot-translator`, v0.6.0,
  MPL-2.0). Prism downloads the matching WebAssembly file and the language
  models from Mozilla's servers the first time they're needed, and checks
  each file's SHA-256.
- `cld-worker.js`, `cld-worker.js.mem`: CLD2, the language detector Firefox
  uses (Apache-2.0, see `LICENSE-cld2`).
- `fzstd.js`: decompresses the downloaded files (MIT, notice inside).

The part that runs in web pages is `../translate-page.js`.
