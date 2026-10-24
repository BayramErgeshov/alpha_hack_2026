#!/usr/bin/env python3
"""Browser test of the whole jury scenario on the real backend with a local mock model.

Usage (project root): python3 -I tests/ui/ui_e2e.py [build/alpha_backend]
Needs Python Playwright with Chromium (pip install playwright && playwright install chromium).
Exits 0 with a SKIP note when Playwright is not installed, so CI without a browser stays green.
"""
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("SKIP: python playwright is not installed")
    sys.exit(0)

spec = importlib.util.spec_from_file_location("e2e", os.path.join(ROOT, "tests", "e2e_test.py"))
e2e = importlib.util.module_from_spec(spec)
sys.argv = [sys.argv[0], sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "alpha_backend")]
spec.loader.exec_module(e2e)

CHECKS = []


def check(name, condition, detail=""):
    CHECKS.append((name, bool(condition), detail))
    print(("[ OK ] " if condition else "[FAIL] ") + name + ("" if condition else f": {detail}"))


def main():
    srv, mock, _ = e2e.mock_server("happy", {"ALFA_STATIC_DIR": os.path.join(ROOT, "ui")})
    base = "http://127.0.0.1:%d" % srv.port
    errors = []
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 1440, "height": 900})
            page.on("pageerror", lambda e: errors.append(str(e)))
            page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)

            page.goto(base + "/")
            page.wait_for_selector("#fan-count:not(:text('—'))", timeout=20000)
            check("home screen is the start page", page.is_visible("#home-panel") and not page.is_visible("#map-panel"))
            check("home has the entry tile", page.is_visible("#home-entry") and "Новости для бизнеса" in page.inner_text("#home-entry"))
            check("home shows the top event", len(page.inner_text("#home-top-title")) > 10)

            page.click("#home-entry")
            page.wait_for_selector("#map-workspace:not([hidden])", timeout=20000)
            check("entry tile opens the influence map", page.is_visible("#map-panel"))
            first_cfo = page.inner_text("#event-nodes")
            effects = [page.inner_text(f"#effects .effect >> nth={i}").split("\n")[0] for i in range(3)]
            check("influence topics are not all the same", len(set(effects)) > 1 or effects[0] != "Ставки и кредитование", effects)

            page.select_option("#role", "accountant")
            page.wait_for_function("document.querySelector('#feed-meta').textContent.includes('бухгалтер')", timeout=20000)
            check("role change rebuilds the feed", page.inner_text("#event-nodes") != first_cfo)

            page.click("#choose-company")
            page.wait_for_selector("#clients .client >> nth=3", timeout=20000)
            before = page.inner_text("#graph-company")
            page.click("#clients .client >> nth=3")
            page.wait_for_function(f"document.querySelector('#graph-company').textContent !== '{before}'", timeout=20000)
            check("company change rebuilds the map", page.inner_text("#graph-company") != before)

            page.click("#map-open")
            page.wait_for_selector("#insight .block", timeout=30000)
            text = page.inner_text("#insight")
            for label in ("Что произошло", "Почему это важно вам", "Риски", "Позиция Альфа-Банка", "Как агент пришёл к выводу", "Источники"):
                check(f"insight shows '{label}'", label in text)
            check("AI conclusions are labelled", page.locator("#insight .label.ai").count() >= 2)
            check("evidence links present", page.locator("#insight .ev").count() >= 3)
            check("conclusions based on bank analytics are marked", page.locator("#insight .bank-basis").count() >= 1)

            page.click("text=Подробнее о рисках")
            page.wait_for_selector("#answers .answer b", timeout=20000)
            check("lens follow-up answered", len(page.inner_text("#answers")) > 20)

            page.get_by_role("button", name="Да", exact=True).click()
            page.wait_for_selector("text=Спасибо, оценка учтена", timeout=10000)
            check("feedback recorded", True)

            page.click("#tab-scenarios")
            page.wait_for_selector("#scenario-panel:not([hidden])", timeout=5000)
            page.fill("#amount", "2000000")
            page.fill("#base-rate", "80")
            page.fill("#scenario-rate", "90")
            page.dispatch_event("#scenario-rate", "input")
            check("scenario calculator computes the delta", "+20" in page.inner_text("#delta-value"), page.inner_text("#delta-value"))

            page.click("#tab-home")
            page.wait_for_function("location.hash === '#home'", timeout=5000)
            page.wait_for_timeout(200)
            check("home hides the insight panel", not page.is_visible("#insight-section"))

            page.set_viewport_size({"width": 390, "height": 844})
            page.goto(base + "/#map")
            page.wait_for_selector("#map-workspace:not([hidden])", timeout=20000)
            width = page.evaluate("document.documentElement.scrollWidth")
            check("no horizontal scroll on a phone", width <= 392, width)
            browser.close()
        check("no JavaScript errors", not errors, errors)
    finally:
        srv.stop()
        mock.terminate()
    failed = [c for c in CHECKS if not c[1]]
    print("%d/%d UI checks passed" % (len(CHECKS) - len(failed), len(CHECKS)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
