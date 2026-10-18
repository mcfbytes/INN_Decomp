#!/usr/bin/env python3
"""A MOCK sign-in server for the browser page. FOR LOCAL DEVELOPMENT AND TESTS ONLY, NEVER PRODUCTION.

*** It has no real authentication: the "consent page" has an Allow button and anyone who can reach it ***
*** is signed in. It binds 127.0.0.1 only, keeps everything in memory and prints the passwords it    ***
*** issues. Do not expose it, do not point real players at it, and do not reuse its code as a server. ***

It implements the page <-> server contract in docs/web.md ("Sign-in (server contract)"):

    GET  /auth/providers                     [{id, name, url}]
    GET  /auth/login?provider=&return=&mode=&nonce=
                                             consent page (Allow / Deny)
    GET  /auth/callback?...                   finishes: popup postMessage, or a redirect with a fragment; the
                                              page's nonce comes back with either
    GET  /auth/session                        Bearer token -> {identity: {handle, provider}, accounts, canCreate,
                                              attested: {age, terms} | null}
    POST /auth/attest                         {attest: "18+v1"} -> 204 (records the age/terms answer)
    POST /auth/accounts                       {} or {persona} -> 201 {id, persona|null}; 403 attest_required with
                                              --attest-required; 429 rate_limited with --accounts-per-day
    POST /auth/accounts/<id>/credential       -> {account, password} (rotates)
    GET  /auth/accounts/<id>/saves[?path=P]   Bearer token -> {account, files: [{path, version, hash, size, mtime,
                                              current, data}]}, or one entry with ?path=; the files are the ones
                                              given with --save (the mock never sees the WebSocket)
    POST /auth/logout                         -> 204

Two fake providers, "Example Phone Co." (example) and "Test Telegraph" (telegraph). Each provider has one
identity; --accounts gives accounts to it (default provider: example). Sign-in is PII-free like the real contract: the mock asks for no email, name or profile. Each fake provider
has one subject; the identity key is HMAC-SHA256(secret, issuer + NUL + subject) and the page only ever sees
a handle derived from it ("Caller 7F3A"). The login URL's `attest=<age>+v<terms>` is stored on the identity.
The real innkeeperd now does all of this itself (its docs/server/innkeeperd.md section 8) and binds the
WebSocket to the session; this mock never touches the WebSocket. It differs from the real server in two
ways: its consent page is its own (the real server redirects to the provider's), and the Deny, "Provider
error" and "Server error" buttons stand for the callback's error codes access_denied, provider_error and
server_error. Linking sign-in methods (/auth/identities, innkeeperd.md section 8.7) is not mocked: those
calls are 404, which the page shows as the server's message.

    python3 tools/mock_auth.py --port 2843 --page-origin http://127.0.0.1:2842 \\
        --accounts 100001:Guybrush --enrol open
    # then open http://127.0.0.1:2842/inn.html?auth=http://127.0.0.1:2843
"""
import argparse
import base64
import hashlib
import hmac
import html
import json
import re
import secrets
import string
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

PROVIDERS = {
    "example": "Example Phone Co.",
    "telegraph": "Test Telegraph",
}
ATTEST_RE = re.compile(r"^(\d{1,3})\+v(\d{1,6})$")
PERSONA_RE = re.compile(r"^[A-Za-z0-9]{1,10}$")
PASSWORD_ALPHABET = string.ascii_uppercase + string.digits


class State:
    def __init__(self, accounts, enrol, page_origin, per_day=0, attest_required=False):
        self.lock = threading.Lock()
        self.enrol = enrol
        self.per_day = per_day                  # 0: no limit
        self.attest_required = attest_required
        self.opened = {}        # provider id -> accounts opened through POST /auth/accounts
        self.attest_posted = set()   # session tokens that have POSTed /auth/attest
        self.page_origin = page_origin
        self.secret = secrets.token_bytes(32)   # the HMAC key; random per run, never printed
        self.sessions = {}      # token -> provider id
        self.attested = {}      # identity key -> {age, terms}
        self.accounts = {}      # provider id -> {account id: persona}
        self.passwords = {}     # account id -> last issued password
        self.saves = {}         # account id -> {DOS path: {version, mtime, data}}, from --save
        self.next_id = 100001
        for provider, number, persona in accounts:
            self.accounts.setdefault(provider, {})[number] = persona
            self.next_id = max(self.next_id, number + 1)

    def identity_key(self, provider):
        # HMAC-SHA256(server_secret, issuer + NUL + subject): the raw subject is never stored.
        issuer, subject = f"mock://{provider}", f"mock-subject-{provider}"
        return hmac.new(self.secret, f"{issuer}\0{subject}".encode(), hashlib.sha256).hexdigest()

    def handle(self, provider):
        return "Caller " + self.identity_key(provider)[:4].upper()

    def can_create(self, provider):
        return self.enrol == "open" and not (self.per_day and self.opened.get(provider, 0) >= self.per_day)

    def persona_taken(self, persona):
        low = persona.lower()
        return any(p.lower() == low for owned in self.accounts.values() for p in owned.values())


def parse_accounts(items):
    out = []
    for item in items:
        provider = "example"
        parts = item.split(":")
        if len(parts) == 3:
            provider, parts = parts[0], parts[1:]
        if len(parts) != 2 or not parts[0].isdigit() or provider not in PROVIDERS or (parts[1] and not PERSONA_RE.match(parts[1])):
            raise SystemExit(f"--accounts: bad entry {item!r}; use [provider:]ID:Persona, e.g. 100001:Guybrush (or ID: for an account with no persona yet)")
        out.append((provider, int(parts[0]), parts[1]))
    return out


CONSENT = """<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mock sign-in</title>
<style>body{{font:16px/1.4 system-ui,sans-serif;max-width:30em;margin:3em auto;padding:0 1em}}
.warn{{background:#fde8c8;border:2px solid #b36a10;padding:.5em .8em}}button{{font:inherit;padding:.4em 1.2em;margin-right:.6em}}</style>
<h1>{name}</h1>
<p class="warn">Mock provider for development. Nothing here is real, and Allow signs in anyone.</p>
<p>The game page asks only for an anonymous sign-in (the openid scope): no email, name or profile is shared.</p>
<form method="get" action="/auth/callback">
<input type="hidden" name="provider" value="{provider}"><input type="hidden" name="return" value="{ret}">
<input type="hidden" name="mode" value="{mode}"><input type="hidden" name="attest" value="{attest}">
<input type="hidden" name="nonce" value="{nonce}">
<button name="decision" value="allow" autofocus>Allow</button><button name="decision" value="deny">Deny</button>
<button name="decision" value="provider_error">Provider error</button><button name="decision" value="server_error">Server error</button>
</form>"""

POPUP_DONE = """<!doctype html><meta charset="utf-8"><title>Signed in</title>
<p id="m">One moment.</p>
<script>
var msg = {message}, target = {target};
if (window.opener) {{ window.opener.postMessage(msg, target); window.close(); }}
document.getElementById('m').textContent = window.opener
  ? 'You can close this window.' : 'This window has no game page to talk to. Close it and try again.';
</script>"""


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        server_version = "MockAuth"

        def log_message(self, fmt, *args):
            sys.stderr.write("mock_auth: " + (fmt % args) + "\n")

        # ---- helpers ----
        def cors(self):
            origin = self.headers.get("Origin")
            if origin and origin == state.page_origin:
                self.send_header("Access-Control-Allow-Origin", origin)
                self.send_header("Vary", "Origin")
                self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
                self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type")
                self.send_header("Access-Control-Max-Age", "600")

        def reply(self, status, body=b"", ctype="application/json", headers=()):
            if isinstance(body, (dict, list)):
                body = json.dumps(body).encode()
            elif isinstance(body, str):
                body = body.encode()
            self.send_response(status)
            self.cors()
            self.send_header("Cache-Control", "no-store")
            for k, v in headers:
                self.send_header(k, v)
            if body:
                self.send_header("Content-Type", ctype + ("; charset=utf-8" if ctype.startswith("text") else ""))
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def err(self, status, code, message):
            self.reply(status, {"error": code, "message": message})

        def provider_of(self):
            auth = self.headers.get("Authorization", "")
            token = auth[7:] if auth.startswith("Bearer ") else ""
            return token, state.sessions.get(token)

        def return_ok(self, ret):
            parts = urlsplit(ret)
            return f"{parts.scheme}://{parts.netloc}" == state.page_origin

        # ---- verbs ----
        def do_OPTIONS(self):
            self.reply(204)

        def do_GET(self):
            url = urlsplit(self.path)
            q = {k: v[0] for k, v in parse_qs(url.query).items()}
            if url.path == "/auth/providers":
                return self.reply(200, [{"id": k, "name": v, "url": f"/auth/login?provider={k}"} for k, v in PROVIDERS.items()])
            if url.path == "/auth/login":
                provider, ret = q.get("provider", ""), q.get("return", "")
                if provider not in PROVIDERS:
                    return self.err(400, "bad_provider", "Unknown provider.")
                if not self.return_ok(ret):
                    return self.err(400, "bad_return", "The return address is not an allowed page origin.")
                page = CONSENT.format(name=html.escape(PROVIDERS[provider]), provider=provider,
                                      ret=html.escape(ret, quote=True), mode=html.escape(q.get("mode", "popup"), quote=True),
                                      attest=html.escape(q.get("attest", ""), quote=True),
                                      nonce=html.escape(q.get("nonce", ""), quote=True))
                return self.reply(200, page, "text/html")
            if url.path == "/auth/callback":
                return self.callback(q)
            m = re.fullmatch(r"/auth/accounts/(\d+)/saves", url.path)
            if m:
                token, provider = self.provider_of()
                if not provider:
                    return self.err(401, "unauthorized", "The session is missing or has expired.")
                number = int(m.group(1))
                if number not in state.accounts.get(provider, {}):
                    return self.err(404, "no_such_account", "That account does not belong to this sign-in.")
                held = state.saves.get(number, {})
                entry = lambda path, f: {"path": path, "version": f["version"], "hash": hashlib.sha256(f["data"]).hexdigest(),
                                         "size": len(f["data"]), "mtime": f["mtime"], "current": True}
                if "path" in q:
                    path = q["path"].replace("/", "\\").upper()
                    if path not in held:
                        return self.err(404, "no_such_file", "No such save file.")
                    return self.reply(200, dict(entry(path, held[path]), data=base64.b64encode(held[path]["data"]).decode()))
                return self.reply(200, {"account": number, "files": [
                    dict(entry(path, f), data=base64.b64encode(f["data"]).decode()) for path, f in sorted(held.items())]})
            if url.path == "/auth/session":
                token, provider = self.provider_of()
                if not provider:
                    return self.err(401, "unauthorized", "The session is missing or has expired.")
                owned = state.accounts.get(provider, {})
                return self.reply(200, {
                    "identity": {"handle": state.handle(provider), "provider": provider},
                    "accounts": [{"id": i, "persona": p or None} for i, p in sorted(owned.items())],
                    "canCreate": state.can_create(provider),
                    "attested": state.attested.get(state.identity_key(provider)),
                })
            self.err(404, "not_found", "No such endpoint.")

        def callback(self, q):
            provider, ret, mode = q.get("provider", ""), q.get("return", ""), q.get("mode", "popup")
            if provider not in PROVIDERS or not self.return_ok(ret):
                return self.err(400, "bad_request", "Bad provider or return address.")
            origin = state.page_origin
            if q.get("decision") == "allow":
                token = secrets.token_urlsafe(24)
                with state.lock:
                    state.sessions[token] = provider
                    m = ATTEST_RE.match(q.get("attest", ""))
                    if m:
                        state.attested[state.identity_key(provider)] = {"age": int(m.group(1)), "terms": int(m.group(2))}
                message = {"type": "inn-auth", "session": token}
                frag = f"inn-session={token}"
                sys.stderr.write(f"mock_auth: signed in via {provider}, session {token[:6]}...\n")
            else:
                code = {"provider_error": "provider_error", "server_error": "server_error"}.get(q.get("decision"), "access_denied")
                message = {"type": "inn-auth", "error": code}
                frag = f"inn-error={code}"
            nonce = q.get("nonce", "")
            if re.fullmatch(r"[A-Za-z0-9_-]{16,64}", nonce):
                message["nonce"] = nonce
                frag += f"&inn-nonce={nonce}"
            if mode == "redirect":
                return self.reply(302, headers=[("Location", f"{ret}#{frag}")])
            body = POPUP_DONE.format(message=json.dumps(message).replace("<", "\\u003c"), target=json.dumps(origin))
            self.reply(200, body, "text/html")

        def read_json(self):
            try:
                length = int(self.headers.get("Content-Length") or 0)
                return json.loads(self.rfile.read(length) or b"{}")
            except (ValueError, json.JSONDecodeError):
                return None

        def do_POST(self):
            path = urlsplit(self.path).path
            token, provider = self.provider_of()
            if path == "/auth/logout":
                with state.lock:
                    state.sessions.pop(token, None)
                return self.reply(204)
            if not provider:
                return self.err(401, "unauthorized", "The session is missing or has expired.")
            if path == "/auth/attest":
                data = self.read_json()
                m = ATTEST_RE.match(data.get("attest", "")) if isinstance(data, dict) and isinstance(data.get("attest"), str) else None
                if not m:
                    return self.err(400, "bad_attest", "Use <age>+v<terms>, for example 18+v1.")
                with state.lock:
                    state.attested[state.identity_key(provider)] = {"age": int(m.group(1)), "terms": int(m.group(2))}
                    state.attest_posted.add(token)
                return self.reply(204)
            if path == "/auth/accounts":
                data = self.read_json()
                # No body, {} or a null persona opens an account with no persona yet: the client's own Select
                # Player screen names one. A given persona is checked, as before.
                persona = data.get("persona") if isinstance(data, dict) else None
                if state.enrol != "open":
                    return self.err(403, "enrolment_closed", "New accounts are not being taken.")
                if state.attest_required and token not in state.attest_posted:
                    return self.err(403, "attest_required", "Answer the age and terms notice first.")
                if not state.can_create(provider):
                    return self.err(429, "rate_limited", "This sign-in has opened enough lines for today.")
                if persona is not None and (not isinstance(persona, str) or not PERSONA_RE.match(persona)):
                    return self.err(400, "bad_persona", "Use 1 to 10 letters or digits.")
                with state.lock:
                    if persona is not None and state.persona_taken(persona):
                        return self.err(409, "persona_taken", f"The name {persona} is already in use.")
                    number = state.next_id
                    state.next_id += 1
                    state.accounts.setdefault(provider, {})[number] = persona or ""
                    state.opened[provider] = state.opened.get(provider, 0) + 1
                return self.reply(201, {"id": number, "persona": persona or None})
            m = re.fullmatch(r"/auth/accounts/(\d+)/credential", path)
            if m:
                number = int(m.group(1))
                if number not in state.accounts.get(provider, {}):
                    return self.err(404, "no_such_account", "That account does not belong to this sign-in.")
                password = "".join(secrets.choice(PASSWORD_ALPHABET) for _ in range(8))
                with state.lock:
                    state.passwords[number] = password
                sys.stderr.write(f"mock_auth: credential account={number} password={password}\n")
                return self.reply(200, {"account": number, "password": password})
            self.err(404, "not_found", "No such endpoint.")

    return Handler


def main():
    ap = argparse.ArgumentParser(
        description="MOCK sign-in server for the browser page. DEVELOPMENT AND TESTS ONLY, NEVER PRODUCTION: "
                    "no real authentication, in-memory, prints issued passwords, binds 127.0.0.1.",
        epilog="Contract: docs/web.md, Sign-in (server contract). The real WebSocket is innkeeperd's job.")
    ap.add_argument("--port", type=int, default=2843, help="port on 127.0.0.1 (default 2843)")
    ap.add_argument("--page-origin", default="http://127.0.0.1:8080",
                    help="the game page's origin, allowed by CORS and as the return address (default %(default)s)")
    ap.add_argument("--accounts", default="100001:Guybrush",
                    help="comma-separated [provider:]ID:Persona accounts owned by that provider's identity; "
                         "provider defaults to example; '' for none (default %(default)s)")
    ap.add_argument("--enrol", choices=("open", "closed"), default="open",
                    help="open: canCreate is true and POST /auth/accounts works; closed: 403 (default open)")
    ap.add_argument("--accounts-per-day", type=int, default=0,
                    help="accounts one identity may open through POST /auth/accounts; beyond that canCreate is false "
                         "and the answer is 429 rate_limited (default 0: no limit)")
    ap.add_argument("--attest-required", action="store_true",
                    help="POST /auth/accounts answers 403 attest_required until the session has POSTed /auth/attest, "
                         "although GET /auth/session reports the attestation as current")
    ap.add_argument("--save", action="append", default=[], metavar="ID:DOSPATH=FILE",
                    help="a save file the account holds, served by GET /auth/accounts/<id>/saves, for example "
                         "100001:INN\\ELAINE.DTA=/path/to/file (repeatable; version 1)")
    args = ap.parse_args()
    state = State(parse_accounts([a for a in args.accounts.split(",") if a]), args.enrol, args.page_origin.rstrip("/"),
                  args.accounts_per_day, args.attest_required)
    for item in args.save:
        head, _, source = item.partition("=")
        number, _, dos = head.partition(":")
        with open(source, "rb") as handle:
            state.saves.setdefault(int(number), {})[dos.replace("/", "\\").upper()] = {
                "version": 1, "mtime": int(time.time() * 1000), "data": handle.read()}
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(state))
    sys.stderr.write(f"mock_auth: MOCK server (development only) on http://127.0.0.1:{args.port}, page origin {state.page_origin}\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
