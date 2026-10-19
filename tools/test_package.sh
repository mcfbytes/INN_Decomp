#!/usr/bin/env bash
# Tests the guards of package_web.sh: lifted, native, preloaded and stray-file builds are refused.
#   tools/test_package.sh [<finished public build dir>]
# With a build directory it also packages that build and checks the output; without one only the refusals run
# (they use small fake build directories and never reach esbuild).
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
fail=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }

fake() {   # fake <name> [cache lines...]: a build directory that looks finished
  local d=$tmp/$1; shift
  mkdir -p "$d"; : > "$d/inn.html"; : > "$d/inn.wasm"
  printf 'CMAKE_HOME_DIRECTORY:INTERNAL=/src/inn\nINN_LIFTED_DIR:PATH=/src/inn/private/lifted\nINN_NATIVE:BOOL=OFF\nINN_NATIVE_HW:BOOL=OFF\nINN_WEB_PRELOAD_DIR:PATH=\n' > "$d/CMakeCache.txt"
  for l in "$@"; do sed -i "s|^${l%%:*}:.*|$l|" "$d/CMakeCache.txt"; done
  echo "$d"
}
refuses() {   # refuses <label> <build> <expected text>
  local msg; msg=$("$here/package_web.sh" "$2" "$tmp/out" 2>&1); local rc=$?
  if [ $rc -eq 2 ] && grep -q "$3" <<<"$msg"; then ok "$1"; else bad "$1 (rc=$rc: $msg)"; fi
  [ ! -e "$tmp/out" ] || bad "$1 left an output directory"
}

refuses "refuses INN_NATIVE=ON" "$(fake native INN_NATIVE:BOOL=ON)" INN_NATIVE
refuses "refuses INN_NATIVE_HW=ON" "$(fake hw INN_NATIVE_HW:BOOL=ON)" INN_NATIVE_HW
refuses "refuses a preloaded C: drive" "$(fake preload INN_WEB_PRELOAD_DIR:PATH=/x/inn-c)" INN_WEB_PRELOAD_DIR
d=$(fake lifted-moved); sed -i 's|^INN_LIFTED_DIR:PATH=.*|INN_LIFTED_DIR:PATH=/elsewhere/lifted|' "$d/CMakeCache.txt"
mkdir -p "$tmp/lifted-elsewhere" && sed -i "s|^INN_LIFTED_DIR:PATH=.*|INN_LIFTED_DIR:PATH=$tmp/lifted-elsewhere|" "$d/CMakeCache.txt"
refuses "refuses INN_LIFTED_DIR set to another path that exists" "$d" "INN_LIFTED_DIR exists"
d2=$(fake lifted-default); mkdir -p "$d2/home/private/lifted"; sed -i "s|^INN_LIFTED_DIR:PATH=.*|INN_LIFTED_DIR:PATH=$d2/home/private/lifted|; s|^CMAKE_HOME_DIRECTORY:INTERNAL=.*|CMAKE_HOME_DIRECTORY:INTERNAL=$d2/home|" "$d2/CMakeCache.txt"
refuses "refuses a default INN_LIFTED_DIR that exists" "$d2" "INN_LIFTED_DIR exists"
d=$(fake table); head -c 400 /dev/zero | tr '\0' 'x' > "$d/lifted_table.c"
refuses "refuses a lifted table that is not the stub" "$d" lifted_table
d=$(fake data); : > "$d/inn.data"
refuses "refuses inn.data in the build directory" "$d" "game data"
d=$(fake exe); mkdir -p "$d/c"; : > "$d/c/TSNEXEC.EXE"
refuses "refuses a game executable in the build directory" "$d" "game data"

if [ -n "${1:-}" ]; then
  b=$1
  if "$here/package_web.sh" "$b" "$tmp/pkg" --game wss://play.example.test:2316 --auth https://auth.example.test >"$tmp/log" 2>&1; then ok "packages the public build"; else bad "packages the public build: $(tail -3 "$tmp/log")"; fi
  # nothing outside the expected list, no game data, hashed names, the config is valid and under SWA's 20 KB limit
  stray=$(cd "$tmp/pkg" && find . -type f | sed 's|^\./||' | grep -Ev '^(index\.html|site\.json|og\.png|icons/(favicon\.ico|icon\.svg|apple-touch-icon\.png|icon-(192|512)\.png|manifest\.webmanifest)|staticwebapp\.config\.json|legal/(terms|privacy|copyright)\.html|assets/(app|inn|legal)\.[0-9a-f]{10}\.(js|css|wasm)|assets/vt323\.[0-9a-f]{10}\.ttf|assets/worklet\.[0-9a-f]{10}\.js|assets/(scenes|music)/[a-z]+\.[0-9a-f]{10}\.js|assets/shaders/[A-Za-z0-9_+-]+\.[0-9a-f]{10}\.(glsl|png))$')
  [ -z "$stray" ] && ok "output holds only expected files" || bad "stray files: $stray"
  [ "$(find "$tmp/pkg" -type f | wc -l)" -ge 20 ] && ok "output file count" || bad "output file count"
  python3 -I - "$tmp/pkg/staticwebapp.config.json" <<'P' && ok "staticwebapp.config.json" || bad "staticwebapp.config.json"
import json, sys
p = sys.argv[1]; c = json.load(open(p))
assert len(open(p, 'rb').read()) < 20480
h = c['globalHeaders']
assert "frame-ancestors 'none'" in h['Content-Security-Policy'] and "'unsafe-eval'" not in h['Content-Security-Policy']
assert 'wss://play.example.test:2316' in h['Content-Security-Policy'] and 'https://auth.example.test' in h['Content-Security-Policy']
assert 'wss://auth.example.test' in h['Content-Security-Policy']
assert h['Cross-Origin-Opener-Policy'] == 'same-origin-allow-popups' and h['Referrer-Policy']
assert c['mimeTypes']['.wasm'] == 'application/wasm' and c['mimeTypes']['.webmanifest'] == 'application/manifest+json'
assert "manifest-src 'self'" in h['Content-Security-Policy'] and "img-src 'self'" in h['Content-Security-Policy']
r = {x['route']: x for x in c['routes']}
assert r['/icons/*']['headers']['Cache-Control'] == 'public, max-age=86400'
assert 'immutable' in r['/assets/*']['headers']['Cache-Control'] and r['/index.html']['headers']['Cache-Control'] == 'no-cache' and r['/']['headers']['Cache-Control'] == 'no-cache' and r['/inn.html']['redirect'] == '/' and r['/inn.html']['statusCode'] == 301 and r['/site.json']['headers']['Cache-Control'] == 'no-cache'
P
  # the icons ship: the .ico holds 16, 32 and 48 px images, the manifest parses and names files that exist, and every page's
  # icon links resolve (the legal pages one level down), with no inline icon left and no Sierra or INN name in the manifest
  python3 -I - "$tmp/pkg" <<'P' && ok "the icons ship, the manifest parses and the pages' icon links resolve" || bad "the icons ship, the manifest parses and the pages' icon links resolve"
import json, os, re, struct, sys
d = sys.argv[1]
ico = open(os.path.join(d, 'icons/favicon.ico'), 'rb').read()
assert ico[:4] == b'\0\0\1\0'
n = struct.unpack('<H', ico[4:6])[0]
assert sorted(ico[6 + 16 * i] for i in range(n)) == [16, 32, 48]
for f, w in (('apple-touch-icon.png', 180), ('icon-192.png', 192), ('icon-512.png', 512)):
    p = open(os.path.join(d, 'icons', f), 'rb').read()
    assert p[:8] == b'\x89PNG\r\n\x1a\n' and int.from_bytes(p[16:20], 'big') == w == int.from_bytes(p[20:24], 'big'), f
assert open(os.path.join(d, 'icons/icon.svg')).read().startswith('<svg')
mf = os.path.join(d, 'icons/manifest.webmanifest')
m = json.load(open(mf))
assert m['name'] and m['short_name'] and m['display'] in ('browser', 'standalone', 'minimal-ui', 'fullscreen')
assert re.fullmatch(r'#[0-9a-f]{6}', m['background_color']) and re.fullmatch(r'#[0-9a-f]{6}', m['theme_color'])
assert not re.search(r'sierra|imagination|tsn|\binn\b', json.dumps(m), re.I)
for i in m['icons']:
    assert os.path.isfile(os.path.join(d, 'icons', i['src'])), i
assert os.path.isdir(os.path.normpath(os.path.join(d, 'icons', m['start_url'])))
for f, up in (('index.html', ''), ('legal/terms.html', '../'), ('legal/privacy.html', '../'), ('legal/copyright.html', '../')):
    h = open(os.path.join(d, f)).read()
    assert 'href="data:' not in h and "href=data:" not in h, f
    at = lambda a, k: (re.search(r'\b' + k + r'=(?:"([^"]*)"|([^\s>"]+))', a) or [None] * 3)
    links = [((at(a, 'rel')[1] or at(a, 'rel')[2]), (at(a, 'href')[1] or at(a, 'href')[2])) for a in re.findall(r'<link\b([^>]*)>', h)]
    links = [(k, v) for k, v in links if k in ('icon', 'apple-touch-icon', 'manifest')]
    assert {k for k, _ in links} == {'icon', 'apple-touch-icon', 'manifest'} and len(links) == 4, f
    for _, href in links:
        assert href.startswith(up + 'icons/') and os.path.isfile(os.path.normpath(os.path.join(d, os.path.dirname(f), href))), (f, href)
P
  # the runtime is not in the page: no script tag for it, inn.js is in the asset map (page.js inserts it), the page is the lazy kind
  python3 -I - "$tmp/pkg" <<'P' && ok "index.html loads the runtime lazily" || bad "index.html loads the runtime lazily"
import json, os, re, sys
d = sys.argv[1]; h = open(os.path.join(d, 'index.html')).read()
assert not re.search(r'<script[^>]*src=[^>]*inn\.', h) and '<template' not in h
assert 'data-runtime="lazy"' in h or 'data-runtime=lazy' in h
m = json.loads(re.search(r'id="inn-assets">([^<]*)<', h).group(1))
assert re.fullmatch(r'assets/inn\.[0-9a-f]{10}\.js', m['inn.js']) and os.path.exists(os.path.join(d, m['inn.js']))
assert re.search(r'inn\.[0-9a-f]{10}\.wasm', open(os.path.join(d, m['inn.js'])).read())
P
  # the display filters: every shader and lookup table is a hashed asset in the map, the licence headers stay in the GLSL,
  # and the page's scripts carry no remote loads or eval
  python3 -I - "$tmp/pkg" <<'P' && ok "the display-filter shaders are hashed assets" || bad "the display-filter shaders are hashed assets"
import json, os, re, sys
d = sys.argv[1]; h = open(os.path.join(d, 'index.html')).read()
m = json.loads(re.search(r'id="inn-assets">([^<]*)<', h).group(1))
sh = {k: v for k, v in m.items() if k.startswith('shaders/')}
for need in ('xbr-lv2.glsl', 'xbrz-freescale.glsl', 'scalefx-pass0.glsl', 'hqx-pass2.glsl', 'hq4x.png', 'fsr-pass0.glsl', 'crt-lottes.glsl', 'supereagle.glsl', '2xsai.glsl', 'Anime4K_Upscale_CNN_x2_S.glsl'):
    assert 'shaders/' + need in sh, need
for k, v in sh.items():
    assert re.fullmatch(r'assets/shaders/[\w+-]+\.[0-9a-f]{10}\.(glsl|png)', v) and os.path.exists(os.path.join(d, v)), k
    if v.endswith('.glsl'):
        t = open(os.path.join(d, v)).read()
        assert re.search(r'copyright|public domain|license|licence', t, re.I), k + ' lost its licence header'
app = [f for f in os.listdir(os.path.join(d, 'assets')) if f.startswith('app.')][0]
js = open(os.path.join(d, 'assets', app)).read()
assert 'Filters' in js and 'glcanvas' in js and not re.search(r'\beval\s*\(|new Function', js)
P
  # the audio worklet module is a hashed same-origin asset in the map (script-src 'self' covers addModule)
  python3 -I - "$tmp/pkg" <<'P' && ok "the audio worklet is a hashed asset" || bad "the audio worklet is a hashed asset"
import json, os, re, sys
d = sys.argv[1]; h = open(os.path.join(d, 'index.html')).read()
m = json.loads(re.search(r'id="inn-assets">([^<]*)<', h).group(1))
w = m['audio-worklet.js']
assert re.fullmatch(r'assets/worklet\.[0-9a-f]{10}\.js', w)
src = open(os.path.join(d, w)).read()
assert 'registerProcessor' in src and 'inn-sink' in src
assert 'audio-worklet.js' in open(os.path.join(d, m['inn.js'])).read()
P
  # the font is served from the page's own origin: hashed asset, CSP font-src 'self', every page's CSS names it, and no
  # packaged file mentions a Google font host
  python3 -I - "$tmp/pkg" <<'P' && ok "the font is self-hosted and nothing names googleapis or gstatic" || bad "the font is self-hosted and nothing names googleapis or gstatic"
import json, os, re, sys
d = sys.argv[1]
fonts = [f for f in os.listdir(os.path.join(d, 'assets')) if re.fullmatch(r'vt323\.[0-9a-f]{10}\.ttf', f)]
assert len(fonts) == 1 and open(os.path.join(d, 'assets', fonts[0]), 'rb').read(4) == b'\x00\x01\x00\x00'
css = [f for f in os.listdir(os.path.join(d, 'assets')) if f.endswith('.css')]
assert css and all(fonts[0] in open(os.path.join(d, 'assets', f)).read() for f in css)
c = json.load(open(os.path.join(d, 'staticwebapp.config.json')))
csp = c['globalHeaders']['Content-Security-Policy']
assert "font-src 'self';" in csp and "style-src 'self';" in csp
for root, _, files in os.walk(d):
    for f in files:
        t = open(os.path.join(root, f), 'rb').read().lower()
        assert b'googleapis' not in t and b'gstatic' not in t, os.path.join(root, f)
P
  # no address given, so no canonical link, robots.txt or sitemap (and the legal pages stay noindex); the preview image still ships
  python3 -I - "$tmp/pkg" <<'P' && ok "without a site address: no canonical, robots.txt or sitemap.xml" || bad "without a site address: no canonical, robots.txt or sitemap.xml"
import os, re, sys
d = sys.argv[1]
for f in ('robots.txt', 'sitemap.xml'): assert not os.path.exists(os.path.join(d, f)), f
assert os.path.getsize(os.path.join(d, 'og.png')) > 1000
for f in ('index.html', 'legal/terms.html', 'legal/privacy.html', 'legal/copyright.html'):
    h = open(os.path.join(d, f)).read()
    assert 'rel="canonical"' not in h and 'og:url' not in h and 'og:image' not in h, f
    assert 'rel=canonical' not in h and '<title>' in h and 'name="description"' in h, f
assert 'content="noindex"' in open(os.path.join(d, 'legal/privacy.html')).read()
P
  # with an address and a connection string: the tags, robots.txt, sitemap.xml and the CSP entry for the ingestion origin
  cp -r "$b" "$tmp/seo" && python3 -I - "$tmp/seo/site.json" <<'P'
import json, sys
d = json.load(open(sys.argv[1])); d['siteName'] = 'Example Site'
d['telemetry'] = {'connectionString': 'InstrumentationKey=00000000-1111-2222-3333-444444444444;IngestionEndpoint=https://eastus-8.in.applicationinsights.azure.com/;LiveEndpoint=https://eastus.livediagnostics.monitor.azure.com/'}
json.dump(d, open(sys.argv[1], 'w'))
P
  if "$here/package_web.sh" "$tmp/seo" "$tmp/pkg2" --game wss://play.example.test:2316 --site-url https://example.org >"$tmp/log2" 2>&1; then ok "packages with --site-url"; else bad "packages with --site-url: $(tail -3 "$tmp/log2")"; fi
  python3 -I - "$tmp/pkg2" <<'P' && ok "search and preview tags, robots.txt, sitemap.xml, telemetry CSP" || bad "search and preview tags, robots.txt, sitemap.xml, telemetry CSP"
import json, os, re, sys
d = sys.argv[1]
rd = lambda f: open(os.path.join(d, f)).read()
assert rd('robots.txt') == 'User-agent: *\nAllow: /\n\nSitemap: https://example.org/sitemap.xml\n'
assert re.findall(r'<loc>([^<]*)</loc>', rd('sitemap.xml')) == ['https://example.org/', 'https://example.org/legal/terms.html', 'https://example.org/legal/privacy.html', 'https://example.org/legal/copyright.html']
png = open(os.path.join(d, 'og.png'), 'rb').read()
assert png[:8] == b'\x89PNG\r\n\x1a\n' and int.from_bytes(png[16:20], 'big') == 1200 and int.from_bytes(png[20:24], 'big') == 630
for f, path in (('index.html', '/'), ('legal/terms.html', '/legal/terms.html'), ('legal/privacy.html', '/legal/privacy.html'), ('legal/copyright.html', '/legal/copyright.html')):
    h = rd(f); url = 'https://example.org' + path
    assert f'<link rel="canonical" href="{url}">' in h, f
    assert f'<meta property="og:url" content="{url}">' in h, f
    assert 'content="https://example.org/og.png"' in h and h.count('twitter:card') == 1 and 'summary_large_image' in h, f
    assert re.search(r'<title>[^<]*Example Site</title>' if f != 'index.html' else r'<title>Example Site: [^<]+</title>', h), f
    assert 'name="description"' in h and 'name="theme-color"' in h and 'og:title' in h and 'twitter:image' in h, f
    assert 'content="noindex"' not in h, f
h = rd('index.html')
assert len(re.findall(r'<h1\b', h)) == 1 and '<noscript>' in h
ld = json.loads(re.search(r'<script type="application/ld\+json">(.*?)</script>', h).group(1))
assert ld['@type'] == 'WebApplication' and ld['applicationCategory'] == 'GameApplication' and ld['name'] == 'Example Site' and ld['image'] == 'https://example.org/og.png'
assert not re.search(r'sierra|imagination|tsn|\binn\b', json.dumps(ld) + re.search(r'<title>.*?</title>', h).group(0), re.I)
c = json.load(open(os.path.join(d, 'staticwebapp.config.json')))
csp = c['globalHeaders']['Content-Security-Policy']
assert 'https://eastus-8.in.applicationinsights.azure.com' in csp and 'livediagnostics' not in csp
cs = csp.split('connect-src ')[1].split(';')[0].split()
assert 'https://eastus-8.in.applicationinsights.azure.com' in cs and not {'https:', 'wss:', 'ws:', '*'} & set(cs), cs
assert "script-src 'self' 'wasm-unsafe-eval' 'sha256-" in csp and "'unsafe-inline'" not in csp.split('script-src')[1].split(';')[0]
r = {x['route']: x for x in c['routes']}
assert 'rewrite' not in r['/'] and '/robots.txt' in r and '/sitemap.xml' in r and '/og.png' in r
P
  # refusals: a bad connection string, and a site address with a path
  cp -r "$b" "$tmp/badcs" && python3 -I -c "
import json,sys; p=sys.argv[1]; d=json.load(open(p)); d['telemetry']={'connectionString':'InstrumentationKey=nope;IngestionEndpoint=https://x.test/'}; json.dump(d,open(p,'w'))" "$tmp/badcs/site.json"
  msg=$("$here/package_web.sh" "$tmp/badcs" "$tmp/out5" 2>&1); [ $? -eq 2 ] && grep -q "InstrumentationKey" <<<"$msg" && ok "refuses a bad telemetry connection string" || bad "bad connection string: $msg"
  msg=$("$here/package_web.sh" "$b" "$tmp/out6" --site-url https://example.org/some/path 2>&1); [ $? -eq 2 ] && grep -q "without a path" <<<"$msg" && ok "refuses a --site-url with a path" || bad "site-url path: $msg"
  # a page built for a preloaded drive (it loads the runtime at once and its build holds data) is refused
  cp -r "$b" "$tmp/eager" && sed -i 's|data-runtime=\("\?\)lazy|data-runtime=\1eager|' "$tmp/eager/inn.html"
  msg=$("$here/package_web.sh" "$tmp/eager" "$tmp/out4" 2>&1); [ $? -eq 2 ] && grep -q "lazy-runtime" <<<"$msg" && ok "refuses a page that loads the runtime at once" || bad "eager page: $msg"
  # a stray file among the lazy scenes is refused
  cp -r "$b" "$tmp/stray" && : > "$tmp/stray/scenes/notes.txt"
  msg=$("$here/package_web.sh" "$tmp/stray" "$tmp/out2" 2>&1); [ $? -eq 2 ] && grep -q "unexpected file" <<<"$msg" && ok "refuses an unexpected file among the scenes" || bad "stray scene file: $msg"
  # the sealed copy of the cache must not be edited by the test above
  cp -r "$b" "$tmp/native" && sed -i 's|^INN_NATIVE:BOOL=.*|INN_NATIVE:BOOL=ON|' "$tmp/native/CMakeCache.txt"
  msg=$("$here/package_web.sh" "$tmp/native" "$tmp/out3" 2>&1); [ $? -eq 2 ] && ok "refuses the real build flipped to native" || bad "native flip: $msg"
fi
exit $fail
