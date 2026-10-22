#!/usr/bin/env python3
"""End-to-end tests: start the real backend binary and call it over HTTP.

Usage (from the project root):  python3 -I tests/e2e_test.py [build/alpha_backend] [filter]
A local OpenAI-compatible mock model (tests/mock_llm.py) is started for agent scenarios.
Only the Python standard library is used.
"""
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "alpha_backend")
FILTER = sys.argv[2] if len(sys.argv) > 2 else ""
TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, extra_env=None):
        self.port = free_port()
        env = {k: v for k, v in os.environ.items() if not k.startswith(("ALFA_", "OPENAI_"))}
        env.update({"ALFA_PORT": str(self.port), "ALFA_HOST": "127.0.0.1",
                    "ALFA_NEWS_REFRESH_MINUTES": "0", "ALFA_WORK_DIR": os.path.join(ROOT, "build", "e2e-work-%d" % self.port)})
        env.update(extra_env or {})
        self.proc = subprocess.Popen([BINARY], cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                if self.get("/health")[0] == 200:
                    return
            except OSError:
                time.sleep(0.3)
        raise RuntimeError("server did not start")

    def request(self, method, path, body=None, headers=None):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (self.port, path), data=data, method=method,
                                     headers={"Content-Type": "application/json", **(headers or {})})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                raw = r.read()
                ctype = r.headers.get("Content-Type", "")
                return r.status, (json.loads(raw) if "json" in ctype else raw.decode("utf-8", "replace")), dict(r.headers)
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                return e.code, json.loads(raw), dict(e.headers)
            except ValueError:
                return e.code, raw.decode("utf-8", "replace"), dict(e.headers)

    def get(self, path, headers=None):
        return self.request("GET", path, headers=headers)

    def post(self, path, body, headers=None):
        return self.request("POST", path, body, headers)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


SHARED = {}


def server():
    if "plain" not in SHARED:
        SHARED["plain"] = Server()
    return SHARED["plain"]


# ---------------------------------------------------------------- clients
@test
def clients_endpoint_lists_large_companies():
    status, body, _ = server().get("/api/v1/clients?limit=5")
    assert status == 200, body
    assert len(body["items"]) == 5
    assert all(i["revenue_rub"] >= 15e9 for i in body["items"])
    status, facets, _ = server().get("/api/v1/clients/facets")
    assert status == 200 and facets["industries"], facets
    industry = facets["industries"][0]["name"]
    status, sub, _ = server().get("/api/v1/clients?industry=" + urllib.request.quote(industry))
    assert status == 200 and all(i["industry"] == industry for i in sub["items"]), sub


# ---------------------------------------------------------------- news and feed
@test
def feed_endpoint_changes_with_company_and_role():
    srv = server()
    status, roles, _ = srv.get("/api/v1/roles")
    assert status == 200 and len(roles["items"]) == 5, roles
    status, news, _ = srv.get("/api/v1/news?limit=50")
    assert status == 200 and len(news["items"]) >= 30, news
    _, clients, _ = srv.get("/api/v1/clients?limit=60")
    feeds = {}
    for c in clients["items"]:
        status, feed, _ = srv.get("/api/v1/companies/%s/feed?role=cfo&limit=8" % c["id"])
        assert status == 200, feed
        feeds[c["id"]] = [i["news_id"] for i in feed["items"]]
    assert len(set(tuple(v) for v in feeds.values())) > 3, "feed must differ between companies"
    first = clients["items"][0]["id"]
    _, a, _ = srv.get("/api/v1/companies/%s/feed?role=treasurer&limit=31" % first)
    _, b, _ = srv.get("/api/v1/companies/%s/feed?role=accountant&limit=31" % first)
    assert [i["news_id"] for i in a["items"]] != [i["news_id"] for i in b["items"]], "role must change the feed"
    item = a["items"][0]
    for key in ("title", "url", "why_for_you", "relevance", "categories", "bank_hint"):
        assert key in item, key
    status, missing, _ = srv.get("/api/v1/companies/U_NOPE_0/feed")
    assert status == 404, missing


@test
def news_refresh_reports_each_source():
    status, report, _ = server().post("/api/v1/news/refresh", {})
    assert status == 200, report
    assert len(report["sources"]) == 6 and "added" in report, report


def main():
    failed = 0
    ran = 0
    try:
        for fn in TESTS:
            if FILTER and FILTER not in fn.__name__:
                continue
            ran += 1
            try:
                fn()
                print("[ OK ] " + fn.__name__)
            except Exception as e:  # noqa: BLE001 - report every failure
                failed += 1
                print("[FAIL] %s: %r" % (fn.__name__, e))
    finally:
        for s in SHARED.values():
            s.stop()
    print("%d/%d e2e tests passed" % (ran - failed, ran))
    return 0 if failed == 0 and ran else 1


if __name__ == "__main__":
    sys.exit(main())
