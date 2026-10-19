#!/usr/bin/env python3
"""Tests tools/make_site_json.py:  python3 -I tools/test_make_site_json.py"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "make_site_json.py")
REPO_SITE = os.path.join(os.path.dirname(HERE), "runtime", "web", "site.json")

FULL = {
    "SITE_NAME": "Example Online", "SITE_OPERATOR_NAME": "Example Operator LLC",
    "SITE_CONTACT_EMAIL": "contact@example.test", "SITE_PRIVACY_EMAIL": "privacy@example.test",
    "SITE_DMCA_NAME": "Pat Agent", "SITE_DMCA_ADDRESS": "1 Main St, Town, ST 00000",
    "SITE_DMCA_EMAIL": "dmca@example.test", "SITE_DMCA_PHONE": "+1 555 0100",
    "SITE_GOVERNING_LAW": "State of Example", "SITE_SERVER_LOCATION": "Example Region",
    "SITE_EFFECTIVE_DATE": "2026-01-01", "SITE_TERMS_VERSION": "3", "SITE_MINIMUM_AGE": "16",
    "SITE_AUDIT_RETENTION_DAYS": "30", "SITE_TELEMETRY_RETENTION_DAYS": "45",
    "SITE_AUTH_SERVER": "https://game.example.test", "SITE_GAME_SERVER": "wss://game.example.test:2316",
    "SITE_URL": "https://play.example.test",
    "SITE_TELEMETRY_CONNECTION_STRING": "InstrumentationKey=00000000-0000-0000-0000-000000000000;IngestionEndpoint=https://x.example.test/",
}


def run(env):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "site.json")
        p = subprocess.run([sys.executable, "-I", TOOL, out], env=env, capture_output=True, text=True)
        data = None
        if os.path.exists(out):
            with open(out, encoding="utf-8") as f:
                raw = f.read()
            data = (json.loads(raw), raw)
        return p, data


class T(unittest.TestCase):
    def test_defaults_match_the_repo_site_json(self):
        p, data = run({})
        self.assertEqual(p.returncode, 0, p.stderr)
        site, _ = data
        with open(REPO_SITE) as f:
            repo = json.load(f)
        for k, v in repo.items():
            self.assertEqual(site[k], v, k)
        self.assertEqual(site["auditRetentionDays"], 90)
        self.assertEqual(site["telemetryRetentionDays"], 90)
        self.assertEqual(site["authServer"], "")
        self.assertEqual(site["telemetry"], {"connectionString": ""})
        self.assertIn("empty: siteName", p.stdout)

    def test_blank_values_are_defaults(self):
        p, data = run({"SITE_MINIMUM_AGE": "  ", "SITE_NAME": "  "})
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(data[0]["minimumAge"], 18)
        self.assertEqual(data[0]["siteName"], "")

    def test_full_set_and_key_order(self):
        p, data = run(FULL)
        self.assertEqual(p.returncode, 0, p.stderr)
        site, _ = data
        self.assertEqual(list(site), ["siteName", "operatorName", "contactEmail", "privacyEmail", "dmcaAgent",
                                      "governingLaw", "serverLocation", "effectiveDate", "termsVersion", "minimumAge",
                                      "auditRetentionDays", "telemetryRetentionDays", "authServer", "gameServer",
                                      "siteUrl", "telemetry"])
        self.assertEqual(list(site["dmcaAgent"]), ["name", "address", "email", "phone"])
        self.assertEqual(site["dmcaAgent"]["phone"], "+1 555 0100")
        self.assertEqual((site["termsVersion"], site["minimumAge"], site["auditRetentionDays"],
                          site["telemetryRetentionDays"]), (3, 16, 30, 45))
        self.assertEqual(site["gameServer"], "wss://game.example.test:2316")
        self.assertEqual(site["siteUrl"], "https://play.example.test")
        self.assertIn("InstrumentationKey=", site["telemetry"]["connectionString"])
        self.assertIn("empty: none", p.stdout)
        self.assertNotIn("InstrumentationKey", p.stdout)

    def test_strips_whitespace_and_trailing_slash(self):
        p, data = run({"SITE_NAME": "  Example  ", "SITE_URL": " https://play.example.test/ "})
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(data[0]["siteName"], "Example")
        self.assertEqual(data[0]["siteUrl"], "https://play.example.test")

    def test_unicode_and_quotes_are_kept(self):
        p, data = run({"SITE_OPERATOR_NAME": 'Café "Inn" \\ Co'})
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(data[0]["operatorName"], 'Café "Inn" \\ Co')

    def rejects(self, var, value, hidden=False):
        p, data = run({var: value})
        self.assertEqual(p.returncode, 2, f"{var}={value!r}: {p.stdout}{p.stderr}")
        self.assertIsNone(data, f"{var} left an output file")
        self.assertIn(var, p.stderr)
        if hidden:
            self.assertNotIn(value.strip(), p.stderr)

    def test_rejects_bad_integers(self):
        for var in ("SITE_TERMS_VERSION", "SITE_MINIMUM_AGE", "SITE_AUDIT_RETENTION_DAYS",
                    "SITE_TELEMETRY_RETENTION_DAYS"):
            for v in ("0", "-1", "1.5", "abc", "1e3", "+4", "99999999999"):
                self.rejects(var, v)

    def test_rejects_bad_urls(self):
        for var, vals in (("SITE_AUTH_SERVER", ["http://a.example", "wss://a.example", "https://a.example/x",
                                                "https://a.example?q=1", "https://u:p@a.example", "a.example",
                                                "https://", "https://a.example#f", "https://a.example:99999"]),
                          ("SITE_GAME_SERVER", ["ws://a.example", "https://a.example", "wss://a.example/path",
                                                "wss://u@a.example"]),
                          ("SITE_URL", ["http://a.example", "https://a.example/app", "https://a.example/?x",
                                        "https://user@a.example"])):
            for v in vals:
                self.rejects(var, v)

    def test_rejects_bad_emails(self):
        for var in ("SITE_CONTACT_EMAIL", "SITE_PRIVACY_EMAIL", "SITE_DMCA_EMAIL"):
            for v in ("nobody", "a@b", "a b@c.test", "a@b.test, c@d.test", "<a@b.test>"):
                self.rejects(var, v)

    def test_rejects_control_characters(self):
        for var in ("SITE_NAME", "SITE_OPERATOR_NAME", "SITE_DMCA_ADDRESS", "SITE_GOVERNING_LAW", "SITE_URL",
                    "SITE_MINIMUM_AGE", "SITE_DMCA_PHONE"):
            self.rejects(var, "a\nb")
            self.rejects(var, "a\tb")

    def test_connection_string_is_never_echoed(self):
        self.rejects("SITE_TELEMETRY_CONNECTION_STRING", "InstrumentationKey=SECRETISH\nx", hidden=True)
        self.rejects("SITE_TELEMETRY_CONNECTION_STRING", "SECRETISH\x07", hidden=True)

    def test_usage(self):
        p = subprocess.run([sys.executable, "-I", TOOL], capture_output=True, text=True, env={})
        self.assertEqual(p.returncode, 64)


if __name__ == "__main__":
    unittest.main(verbosity=1)
