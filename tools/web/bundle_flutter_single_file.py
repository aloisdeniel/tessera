#!/usr/bin/env python3
"""Bundle a Flutter web build into ONE self-contained HTML file.

Made for publishing the flutter_tessera example as a hosted single page
(e.g. a Claude artifact) where no other files can be served and a strict CSP
blocks every external host. Everything the app requests at runtime is embedded:

  * main.dart.js, canvaskit.js and tessera_web.js are inlined as plain
    <script> tags (no dynamic script injection, so no loader is needed);
  * every asset — AssetManifest, fonts, images, sounds, the tessera and
    canvaskit .wasm blobs — rides along base64-encoded in a FILES map, and a
    window.fetch patch serves them from memory;
  * the Flutter loader (flutter.js / flutter_bootstrap.js) is replaced by a
    tiny stub: window.flutterCanvasKit is initialised up-front from the
    embedded wasm, and _flutter.loader.didCreateEngineInitializer boots the
    engine directly (no service worker, no gstatic CanvasKit fetch);
  * requests to fonts.gstatic.com (the engine's Roboto fallback) are answered
    with the bundled Roboto so no request ever leaves the page.

Usage:
  bundle_flutter_single_file.py <build/web dir> <out.html> [--title "..."]
"""
import base64
import json
import pathlib
import re
import sys

MIME = {
    '.js': 'text/javascript',
    '.json': 'application/json',
    '.bin': 'application/octet-stream',
    '.wasm': 'application/wasm',
    '.ttf': 'font/ttf',
    '.otf': 'font/otf',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.wav': 'audio/wav',
    '.frag': 'application/octet-stream',
    '.wgsl': 'text/plain',
}

# Runtime-fetched paths answered from the FILES map under a different key.
ALIASES = {
    # Inlined scripts resolve their wasm against the page root.
    'tessera_web.wasm': 'assets/packages/flutter_tessera/assets/web/tessera_web.wasm',
    'canvaskit.wasm': 'canvaskit/canvaskit.wasm',
    # The engine's downloadable Roboto fallback (fonts.gstatic.com).
    '__fallback_font__': 'assets/assets/fonts/Roboto-Regular.ttf',
}

SKIP = {
    'assets/NOTICES',  # licence text, never shown by the example
    'assets/packages/flutter_tessera/assets/web/tessera_web.js',  # inlined
}


def esc_inline(js: str) -> str:
    """Make raw JS safe inside a <script> tag."""
    return js.replace('</script', '<\\/script').replace('<!--', '<\\!--')


def main() -> None:
    build = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2])
    title = 'Tessera Games'
    if '--title' in sys.argv:
        title = sys.argv[sys.argv.index('--title') + 1]

    files = {}
    for p in sorted((build / 'assets').rglob('*')):
        if not p.is_file():
            continue
        rel = p.relative_to(build).as_posix()
        if rel in SKIP:
            continue
        files[rel] = [base64.b64encode(p.read_bytes()).decode(),
                      MIME.get(p.suffix, 'application/octet-stream')]
    ck_wasm = build / 'canvaskit' / 'canvaskit.wasm'
    files['canvaskit/canvaskit.wasm'] = [
        base64.b64encode(ck_wasm.read_bytes()).decode(), 'application/wasm']

    main_js = esc_inline((build / 'main.dart.js').read_text())
    # canvaskit.js ships as an ES module; make it a classic script so its
    # top-level `var CanvasKitInit` lands on window (import.meta is only used
    # for the default locateFile, which the boot script overrides anyway).
    ck_js = (build / 'canvaskit' / 'canvaskit.js').read_text()
    ck_js = ck_js.replace('import.meta.url', 'document.baseURI')
    ck_js = re.sub(r'export default CanvasKitInit;?', '', ck_js)
    ck_js = esc_inline(ck_js)
    tessera_js = esc_inline(
        (build / 'assets/packages/flutter_tessera/assets/web/tessera_web.js')
        .read_text())

    boot = (build / 'flutter_bootstrap.js').read_text()
    m = re.search(r'_flutter\.buildConfig = ({.*?});', boot)
    build_config = m.group(1) if m else '{"builds":[]}'

    html = f'''<title>{title}</title>
<style>
  html, body {{ height: 100%; margin: 0; background: #0d1218; overflow: hidden; }}
  #splash {{
    position: fixed; inset: 0; display: flex; flex-direction: column; gap: 14px;
    align-items: center; justify-content: center; color: #9aa6b4;
    font: 14px system-ui, sans-serif; background: #0d1218; z-index: 10;
    text-align: center; padding: 24px;
  }}
  #splash .ring {{
    width: 42px; height: 42px; border-radius: 50%;
    border: 3px solid #263140; border-top-color: #cda45e;
    animation: spin 0.9s linear infinite;
  }}
  #splash b {{ color: #e9e3d4; font-weight: 600; }}
  #splash .warn {{ color: #d98a6a; max-width: 46ch; }}
  @keyframes spin {{ to {{ transform: rotate(360deg); }} }}
</style>
<div id="splash">
  <div class="ring"></div>
  <div><b>{title}</b> — starting the engine…</div>
  <div id="splash-note"></div>
</div>
<script>
window.FILES = {json.dumps(files)};
</script>
<script>
(function () {{
  'use strict';
  var ALIASES = {json.dumps(ALIASES)};
  function bytesOf(entry) {{
    var bin = atob(entry[0]);
    var buf = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
    return buf;
  }}
  var baseDir = new URL('.', document.baseURI).pathname;
  function lookup(url) {{
    var u;
    try {{ u = new URL(url, document.baseURI); }} catch (e) {{ return null; }}
    if (u.hostname === 'fonts.gstatic.com') return ALIASES.__fallback_font__;
    if (u.origin !== location.origin) return null;
    var p = decodeURIComponent(u.pathname);
    if (p.indexOf(baseDir) === 0) p = p.slice(baseDir.length);
    p = p.replace(/^\\/+/, '');
    if (ALIASES[p]) p = ALIASES[p];
    return window.FILES[p] ? p : null;
  }}
  var origFetch = window.fetch.bind(window);
  window.fetch = function (input, init) {{
    var url = (typeof input === 'string') ? input
        : (input && input.url) ? input.url : String(input);
    var p = lookup(url);
    if (p) {{
      var entry = window.FILES[p];
      return Promise.resolve(new Response(bytesOf(entry), {{
        status: 200, headers: {{ 'Content-Type': entry[1] }}
      }}));
    }}
    return origFetch(input, init); // page-runtime traffic; CSP rules the rest
  }};
}})();
</script>
<script>{ck_js}</script>
<script>{tessera_js}</script>
<script>
window._ckReady = CanvasKitInit({{
  locateFile: function (f) {{ return 'canvaskit/' + f; }}
}}).then(function (ck) {{ window.flutterCanvasKit = ck; }});
window._flutter = {{
  buildConfig: {build_config},
  loader: {{
    didCreateEngineInitializer: function (init) {{
      window._ckReady.then(function () {{
        return init.initializeEngine({{ renderer: 'canvaskit' }});
      }}).then(function (engine) {{
        return engine.runApp();
      }}).catch(function (err) {{
        var note = document.getElementById('splash-note');
        if (note) note.innerHTML =
            '<div class="warn">Engine failed to start: ' + err + '</div>';
      }});
    }}
  }}
}};
if (!navigator.gpu) {{
  document.getElementById('splash-note').innerHTML =
      '<div class="warn">This demo renders through WebGPU, which this ' +
      'browser does not expose. Use a recent Chrome or Edge (or Safari 26+) — ' +
      'the menu will load, but the games cannot render.</div>';
}}
window.addEventListener('flutter-first-frame', function () {{
  var s = document.getElementById('splash');
  if (s) s.remove();
}});
</script>
<script>{main_js}</script>
'''
    out.write_text(html)
    print(f'{out}: {out.stat().st_size / 1e6:.1f} MB '
          f'({len(files)} embedded files)')


if __name__ == '__main__':
    main()
