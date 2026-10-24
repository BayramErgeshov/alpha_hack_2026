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


# ---------------------------------------------------------------- agent with a local mock model
def mock_server(scenario, extra_env=None):
    port = free_port()
    log = os.path.join(ROOT, "build", "mock-%s-%d.log" % (scenario, port))
    if os.path.exists(log):
        os.remove(log)
    proc = subprocess.Popen([sys.executable, "-I", os.path.join(ROOT, "tests", "mock_llm.py"), str(port)],
                            env={"MOCK_SCENARIO": scenario, "MOCK_LOG": log, "PATH": os.environ.get("PATH", "")})
    time.sleep(0.8)
    env = {"ALFA_LLM_TEST_ENDPOINT": "1", "ALFA_LLM_BASE_URL": "http://127.0.0.1:%d/v1" % port,
           "ALFA_LLM_API_KEY": "test-key-not-secret"}
    env.update(extra_env or {})
    srv = Server(env)
    return srv, proc, log


def calls(log):
    if not os.path.exists(log):
        return []
    with open(log, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def borrower_and_rate_news(srv):
    _, clients, _ = srv.get("/api/v1/clients?limit=100")
    company = next(c for c in clients["items"] if "loans" in c["products"])
    _, news, _ = srv.get("/api/v1/news?limit=100")
    rate_news = next(n for n in news["items"] if "rate" in n["categories"] and "ставк" in n["title"].lower())
    other = next(n for n in news["items"] if n["id"] != rate_news["id"])
    return company["id"], rate_news["id"], other["id"]


def steps(insight):
    return {s["step"]: s for s in insight["agent_trace"]}


@test
def agent_happy_path_uses_three_models():
    srv, proc, log = mock_server("happy")
    try:
        company, news_id, _ = borrower_and_rate_news(srv)
        status, insight, _ = srv.post("/api/v1/insights", {"company_id": company, "news_id": news_id, "role": "cfo"})
        assert status == 200, insight
        assert insight["generation"]["method"] == "allowlisted_llm", insight["generation"]
        trace = steps(insight)
        for step in ("plan", "tools", "draft", "verify", "finalize"):
            assert step in trace, (step, list(trace))
        assert trace["plan"]["status"] == "complete", trace["plan"]
        assert insight["interpretation"]["bank_position"] is not None, insight["interpretation"]
        assert insight["role"]["id"] == "cfo"
        assert insight["quality_check"]["claims_supported"] >= 1
        interp = insight["interpretation"]
        assert any(c.get("uses_bank_material") for c in interp["opportunities"]), interp["opportunities"]
        assert not any(c.get("uses_bank_material") for c in interp["risks"]), interp["risks"]
        roles = {c["role"]: c["model"] for c in calls(log)}
        assert roles.get("planner") == "google/gemma-4-26b-a4b-it", roles
        assert roles.get("analyst") == "deepseek/deepseek-v4.1-flash", roles
        assert roles.get("reviewer") == "z-ai/glm-5.3-flash", roles
        before = len(calls(log))
        status, again, _ = srv.post("/api/v1/insights", {"company_id": company, "news_id": news_id, "role": "cfo"})
        assert status == 200 and again.get("cached") is True, again.get("cached")
        assert len(calls(log)) == before, "cached insight must not call the model"
        status, answer, _ = srv.post("/api/v1/insights/%s/questions" % insight["id"], {"lens": "treasury"})
        assert status == 200 and answer["answer"], answer
        assert "**" not in answer["answer"] and answer["answer"].startswith("Ликвидность"), answer["answer"]
    finally:
        srv.stop()
        proc.terminate()


@test
def agent_reviewer_requests_more_search_triggers_revise():
    srv, proc, log = mock_server("revise")
    try:
        company, news_id, _ = borrower_and_rate_news(srv)
        status, insight, _ = srv.post("/api/v1/insights", {"company_id": company, "news_id": news_id, "role": "treasurer"})
        assert status == 200, insight
        trace = steps(insight)
        assert "revise" in trace and trace["revise"]["status"] == "complete", trace.get("revise")
        assert insight["quality_check"]["revise_rounds"] == 1
        assert insight["interpretation"]["bank_position"] is not None
        analysts = [c for c in calls(log) if c["role"] == "analyst"]
        assert len(analysts) == 2, analysts
    finally:
        srv.stop()
        proc.terminate()


@test
def agent_broken_model_reply_falls_back():
    srv, proc, log = mock_server("broken")
    try:
        company, news_id, _ = borrower_and_rate_news(srv)
        status, insight, _ = srv.post("/api/v1/insights", {"company_id": company, "news_id": news_id})
        assert status == 200, insight
        assert insight["generation"]["method"] == "deterministic_fallback", insight["generation"]
        assert steps(insight)["plan"]["status"] == "fallback"
        assert insight["interpretation"]["why_it_matters"]
    finally:
        srv.stop()
        proc.terminate()


@test
def agent_reply_cut_by_token_limit_is_reported():
    srv, proc, log = mock_server("truncated")
    try:
        company, news_id, _ = borrower_and_rate_news(srv)
        status, insight, _ = srv.post("/api/v1/insights", {"company_id": company, "news_id": news_id})
        assert status == 200, insight
        assert insight["generation"]["method"] == "deterministic_fallback", insight["generation"]
        assert "token limit" in insight["generation"]["warning"], insight["generation"]
        assert insight["interpretation"]["why_it_matters"]
    finally:
        srv.stop()
        proc.terminate()


@test
def reviewer_failure_is_visible_in_trace():
    srv, proc, log = mock_server("review_cut")
    try:
        company, news_id, _ = borrower_and_rate_news(srv)
        status, insight, _ = srv.post("/api/v1/insights", {"company_id": company, "news_id": news_id})
        assert status == 200, insight
        verify = steps(insight)["verify"]
        assert verify["detail"]["reviewer_status"] == "fallback", verify
        assert "token limit" in verify["detail"]["reviewer_error"], verify
        assert "рецензент" in verify["decision"], verify["decision"]
    finally:
        srv.stop()
        proc.terminate()


@test
def bank_materials_are_named_when_bank_has_no_direct_position():
    srv, proc, log = mock_server("no_bank_view")
    try:
        company, news_id, _ = borrower_and_rate_news(srv)
        status, insight, _ = srv.post("/api/v1/insights", {"company_id": company, "news_id": news_id})
        assert status == 200, insight
        interp = insight["interpretation"]
        assert interp["bank_position"] is None, interp["bank_position"]
        used = interp["bank_materials_used"]
        sources = {s["id"]: s for s in insight["sources"]}
        assert used and all(u["id"].startswith("A") and u["id"] in sources and u["title"] for u in used), used
        assert not any(u["title"].endswith(".md") for u in used), used  # human titles, not file names
    finally:
        srv.stop()
        proc.terminate()


@test
def agent_budget_exhausted_skips_model():
    srv, proc, log = mock_server("happy", {"ALFA_LLM_DAILY_TOKEN_LIMIT": "1"})
    try:
        company, news_id, other = borrower_and_rate_news(srv)
        srv.post("/api/v1/insights", {"company_id": company, "news_id": news_id})
        before = len(calls(log))
        status, insight, _ = srv.post("/api/v1/insights", {"company_id": company, "news_id": other})
        assert status == 200, insight
        assert len(calls(log)) == before, "no model calls after the budget is exhausted"
        assert any(s["status"] == "budget_exhausted" for s in insight["agent_trace"]), insight["agent_trace"]
    finally:
        srv.stop()
        proc.terminate()


@test
def insight_without_model_by_text():
    _, clients, _ = server().get("/api/v1/clients?limit=1")
    status, insight, _ = server().post("/api/v1/insights", {
        "company_id": clients["items"][0]["id"], "title": "Курс рубля",
        "text": "Курс доллара вырос до 90 рублей, экспортёры продают валюту", "role": "treasurer"})
    assert status == 200, insight
    assert insight["generation"]["method"] == "deterministic_fallback"
    assert insight["role"]["id"] == "treasurer"
    status, bad, _ = server().post("/api/v1/insights", {"company_id": clients["items"][0]["id"], "news_id": "n000000000000"})
    assert status == 404, bad


@test
def money_in_sources_is_readable_in_billions():
    # The model writes "62,3 млрд руб."; the verifier and reviewer must find that number in C1/P1.
    _, clients, _ = server().get("/api/v1/clients?limit=1")
    company = clients["items"][0]
    status, insight, _ = server().post("/api/v1/insights", {
        "company_id": company["id"], "title": "Ставка", "text": "Банк России сохранил ключевую ставку 14%"})
    assert status == 200, insight
    c1 = json.loads(next(s for s in insight["sources"] if s["id"] == "C1")["excerpt"])
    expected = round(company["revenue_rub"] / 1e9, 1)
    assert c1["revenue_bln_rub"] == expected, (c1.get("revenue_bln_rub"), expected)
    assert "payments_in_bln_rub" in c1 and "payments_out_bln_rub" in c1, c1


@test
def platform_port_variable_wins():
    # Hosting platforms (SnapDeploy, Render, Railway) tell the app where to listen via PORT.
    own, platform = free_port(), free_port()
    env = {k: v for k, v in os.environ.items() if not k.startswith(("ALFA_", "OPENAI_"))}
    env.update({"ALFA_PORT": str(own), "PORT": str(platform), "ALFA_HOST": "127.0.0.1", "ALFA_NEWS_REFRESH_MINUTES": "0",
                "ALFA_WORK_DIR": os.path.join(ROOT, "build", "e2e-work-%d" % platform)})
    proc = subprocess.Popen([BINARY], cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline, ok = time.time() + 60, False
        while time.time() < deadline and not ok:
            try:
                with urllib.request.urlopen("http://127.0.0.1:%d/health" % platform, timeout=2) as r:
                    ok = r.status == 200
            except OSError:
                time.sleep(0.3)
        assert ok, "server must listen on PORT when the platform sets it"
    finally:
        proc.terminate()
        proc.wait(5)


# ---------------------------------------------------------------- feedback, metrics, offers, static UI
@test
def feedback_and_metrics_endpoints():
    srv = server()
    _, clients, _ = srv.get("/api/v1/clients?limit=100")
    exporter = next((c for c in clients["items"] if c["foreign_trade"]), clients["items"][0])
    status, insight, _ = srv.post("/api/v1/insights", {"company_id": exporter["id"], "title": "Курс рубля",
                                                     "text": "Курс доллара и юаня вырос, экспортёры и импортёры пересматривают валютные расчёты"})
    assert status == 200, insight
    assert "offers" in insight, list(insight)
    if exporter["foreign_trade"]:
        assert insight["offers"] and all(o["is_advertising"] for o in insight["offers"]), insight["offers"]
    status, ok, _ = srv.post("/api/v1/feedback", {"insight_id": insight["id"], "useful": True})
    assert status == 200, ok
    status, bad, _ = srv.post("/api/v1/feedback", {"insight_id": "insight-0-0", "useful": True})
    assert status == 404, bad
    status, bad, _ = srv.post("/api/v1/feedback", {"insight_id": insight["id"]})
    assert status == 400, bad
    status, metrics, _ = srv.get("/api/v1/metrics")
    assert status == 200, metrics
    assert metrics["insights"]["total"] >= 1 and metrics["feedback"]["total"] >= 1, metrics
    assert "budget" in metrics and "definitions" in metrics


@test
def static_ui_and_path_traversal_blocked():
    ui = os.path.join(ROOT, "build", "e2e-ui")
    os.makedirs(os.path.join(ui, "assets"), exist_ok=True)
    with open(os.path.join(ui, "index.html"), "w", encoding="utf-8") as f:
        f.write("<!doctype html><title>UI</title>")
    with open(os.path.join(ui, "assets", "app.js"), "w", encoding="utf-8") as f:
        f.write("console.log(1)")
    srv = Server({"ALFA_STATIC_DIR": ui, "ALFA_CORS_ORIGIN": "http://localhost:5173"})
    try:
        status, body, headers = srv.get("/")
        assert status == 200 and "<title>UI</title>" in body, (status, body)
        status, body, headers = srv.get("/assets/app.js")
        assert status == 200 and "javascript" in headers.get("Content-Type", ""), headers
        status, body, _ = srv.get("/clients/U_1")
        assert status == 200 and "<title>UI</title>" in body, "SPA routes fall back to index.html"
        for evil in ("/../README.md", "/%2e%2e/README.md", "/assets/..%2f..%2fREADME.md", "/..%5cREADME.md"):
            status, body, _ = srv.get(evil)
            assert status in (400, 404), (evil, status)
            assert "Alpha News Backend" not in str(body), evil
        status, _, headers = srv.get("/api/v1/roles")
        assert headers.get("Access-Control-Allow-Origin") == "http://localhost:5173", headers
        status, _, headers = srv.request("OPTIONS", "/api/v1/insights")
        assert status == 204, status
    finally:
        srv.stop()


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
