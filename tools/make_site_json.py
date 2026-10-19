#!/usr/bin/env python3
"""Writes the page's site.json from environment variables (docs/web.md, "Automated deployment").

    python3 -I tools/make_site_json.py <out-path>

Every value comes from a SITE_* variable; see the table in docs/web.md. Unset or blank variables
take the defaults of runtime/web/site.json (empty strings; 1, 18, 90 and 90 for the numbers).
Exits 2 with a message naming the variable when a value is invalid. The message never repeats
SITE_TELEMETRY_CONNECTION_STRING (it ships in the page, but is kept out of logs anyway).
"""
import json
import os
import re
import sys
from urllib.parse import urlsplit

TEXT = {  # variable -> site.json key (the DMCA agent's fields are keyed by the variable)
    "SITE_NAME": "siteName",
    "SITE_OPERATOR_NAME": "operatorName",
    "SITE_CONTACT_EMAIL": "contactEmail",
    "SITE_PRIVACY_EMAIL": "privacyEmail",
    "SITE_GOVERNING_LAW": "governingLaw",
    "SITE_SERVER_LOCATION": "serverLocation",
    "SITE_EFFECTIVE_DATE": "effectiveDate",
}
DMCA = {"SITE_DMCA_NAME": "name", "SITE_DMCA_ADDRESS": "address",
        "SITE_DMCA_EMAIL": "email", "SITE_DMCA_PHONE": "phone"}
INTS = {"SITE_TERMS_VERSION": ("termsVersion", 1), "SITE_MINIMUM_AGE": ("minimumAge", 18),
        "SITE_AUDIT_RETENTION_DAYS": ("auditRetentionDays", 90),
        "SITE_TELEMETRY_RETENTION_DAYS": ("telemetryRetentionDays", 90)}
EMAILS = {"SITE_CONTACT_EMAIL", "SITE_PRIVACY_EMAIL", "SITE_DMCA_EMAIL"}
HIDDEN = {"SITE_TELEMETRY_CONNECTION_STRING"}   # never echoed
EMAIL_RE = re.compile(r"^[^\s@<>,;]+@[^\s@<>,;]+\.[^\s@<>,;]+$")


class Bad(Exception):
    pass


def get(env, name):
    v = env.get(name, "").strip()
    if re.search(r"[\x00-\x1f\x7f]", v):
        raise Bad(f"{name} contains a control character or a newline"
                  + ("" if name in HIDDEN else f": {v!r}"))
    return v


def origin(name, v, scheme):
    if not v:
        return ""
    try:
        u = urlsplit(v)
        u.port   # raises ValueError on a bad port
    except ValueError:
        raise Bad(f"{name} is not a valid URL: {v}")
    if u.scheme != scheme or not u.hostname:
        raise Bad(f"{name} must be a {scheme}://host origin: {v}")
    if "@" in u.netloc:
        raise Bad(f"{name} must not contain user information: {v}")
    if u.path.strip("/") or u.query or u.fragment or "?" in v or "#" in v:
        raise Bad(f"{name} must be an origin without a path, query or fragment: {v}")
    return f"{u.scheme}://{u.netloc}".lower()


def build(env):
    text = {}
    for var in list(TEXT) + list(DMCA):
        v = get(env, var)
        if v and var in EMAILS and not EMAIL_RE.match(v):
            raise Bad(f"{var} does not look like an email address: {v}")
        text[var] = v
    ints = {}
    for var, (key, default) in INTS.items():
        v = get(env, var)
        if not v:
            ints[key] = default
        elif re.fullmatch(r"[0-9]{1,9}", v) and int(v) >= 1:
            ints[key] = int(v)
        else:
            raise Bad(f"{var} must be a positive integer: {v}")
    auth = origin("SITE_AUTH_SERVER", get(env, "SITE_AUTH_SERVER"), "https")
    game = origin("SITE_GAME_SERVER", get(env, "SITE_GAME_SERVER"), "wss")
    url = origin("SITE_URL", get(env, "SITE_URL"), "https")
    conn = get(env, "SITE_TELEMETRY_CONNECTION_STRING")

    # Same key order as the reference site.json.
    return {
        "siteName": text["SITE_NAME"],
        "operatorName": text["SITE_OPERATOR_NAME"],
        "contactEmail": text["SITE_CONTACT_EMAIL"],
        "privacyEmail": text["SITE_PRIVACY_EMAIL"],
        "dmcaAgent": {k: text[v] for v, k in DMCA.items()},
        "governingLaw": text["SITE_GOVERNING_LAW"],
        "serverLocation": text["SITE_SERVER_LOCATION"],
        "effectiveDate": text["SITE_EFFECTIVE_DATE"],
        "termsVersion": ints["termsVersion"],
        "minimumAge": ints["minimumAge"],
        "auditRetentionDays": ints["auditRetentionDays"],
        "telemetryRetentionDays": ints["telemetryRetentionDays"],
        "authServer": auth,
        "gameServer": game,
        "siteUrl": url,
        "telemetry": {"connectionString": conn},
    }


def empties(site):
    out = []
    for k, v in site.items():
        if isinstance(v, dict):
            out += [f"{k}.{kk}" for kk, vv in v.items() if vv == ""]
        elif v == "":
            out.append(k)
    return out


def main(argv, env):
    if len(argv) != 2:
        print("usage: make_site_json.py <out-path>   (values from SITE_* environment variables)", file=sys.stderr)
        return 64
    try:
        site = build(env)
    except Bad as e:
        print(f"make_site_json: {e}", file=sys.stderr)
        return 2
    with open(argv[1], "w", encoding="utf-8") as f:
        json.dump(site, f, indent=2, ensure_ascii=False)
        f.write("\n")
    e = empties(site)
    print(f"make_site_json: wrote {argv[1]}; empty: {', '.join(e) if e else 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv, os.environ))
