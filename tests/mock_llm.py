#!/usr/bin/env python3
"""Local OpenAI-compatible mock model for end-to-end tests (127.0.0.1 only).

The backend sends a role marker in the system prompt ("Роль агента: планировщик",
"аналитик", "рецензент", "уточнение"); the mock answers each role with valid JSON
built from the evidence ids present in the request, so the real agent loop runs.

Like the real gateway's reasoning models (DeepSeek V4.1 Flash, GLM-5.3-Flash), the mock spends the
whole max_tokens budget on hidden reasoning and returns empty content with finish_reason "length"
unless the request disables reasoning with {"reasoning": {"enabled": false}}.

Scenarios (MOCK_SCENARIO): happy | revise | broken | truncated (analyst reply cut at max_tokens) | review_cut (reviewer reply cut) | no_bank_view (claims cite bank materials, no bank position). Every call is appended to
MOCK_LOG as one JSON line {role, model} so tests can assert which models were used.
"""
import json
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

SCENARIO = os.environ.get("MOCK_SCENARIO", "happy")
LOG = os.environ.get("MOCK_LOG", "")
STATE = {"reviews": 0}


def role_of(system):
    for marker, role in (("планировщик", "planner"), ("рецензент", "reviewer"),
                         ("уточнение", "followup"), ("аналитик", "analyst")):
        if "Роль агента: " + marker in system:
            return role
    return "unknown"


def ids_in(text, prefix):
    return sorted(set(re.findall(r'"(%s\d+)"' % prefix, text)))


def answer(role, user):
    bank = ids_in(user, "A")
    if SCENARIO == "broken" and role in ("analyst", "planner"):
        return "Извините, не могу ответить в формате JSON."
    if role == "planner":
        return json.dumps({"analytics_queries": ["ключевая ставка кредиты", "инфляция прогноз"],
                           "focus": ["стоимость долга"], "need_counterparties": True, "need_related_news": True},
                          ensure_ascii=False)
    if role == "reviewer":
        STATE["reviews"] += 1
        if SCENARIO == "revise" and STATE["reviews"] == 1:
            return json.dumps({"verdict": "revise", "issues": ["Нет позиции банка по ставке"],
                               "additional_queries": ["ставка без изменения риторика жестче"]}, ensure_ascii=False)
        return json.dumps({"verdict": "ok", "issues": [], "additional_queries": []})
    if role == "followup":
        return json.dumps({"answer": "**Ликвидность**: для казначея главный риск — стоимость рефинансирования.",
                           "evidence_ids": ["N1", "C1"]}, ensure_ascii=False)
    revised = "previous_draft" in user
    bank_view = {"text": "Альфа-Банк ожидает сохранения жёсткой политики", "evidence_ids": [bank[-1]]} if bank and (SCENARIO != "revise" or revised) and SCENARIO != "no_bank_view" else None
    return json.dumps({
        "what_happened": {"text": "Банк России сохранил ключевую ставку.", "evidence_ids": ["N1"]},
        "why_it_matters": {"text": "У компании есть кредиты, поэтому стоимость долга остаётся высокой.", "evidence_ids": ["N1", "C1"]},
        "potential_effects": [{"text": "Процентные расходы не снизятся в ближайшие кварталы.", "evidence_ids": ["N1", "C1"], "horizon": "3-6 месяцев"}],
        "risks": [{"text": "Рефинансирование долга останется дорогим.", "evidence_ids": ["N1", "C1"], "confidence": 0.7}],
        "opportunities": [{"text": "Зафиксировать ставку по новым кредитам.", "evidence_ids": ["N1"] + bank[:1]}],
        "actions": [{"text": "Пересчитать стресс-сценарий по долгу.", "evidence_ids": ["C1"]}],
        "bank_view": bank_view,
        "model_conclusions": [{"text": "Эффект зависит от доли плавающих ставок.", "evidence_ids": ["C1"]}],
        "uncertainties": ["Структура долга компании неизвестна."]
    }, ensure_ascii=False)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length) or b"{}")
        messages = body.get("messages", [])
        system = next((m["content"] for m in messages if m.get("role") == "system"), "")
        user = next((m["content"] for m in messages if m.get("role") == "user"), "")
        role = role_of(system)
        if LOG:
            with open(LOG, "a", encoding="utf-8") as f:
                f.write(json.dumps({"role": role, "model": body.get("model")}) + "\n")
        reasoning_off = (body.get("reasoning") or {}).get("enabled") is False
        if not reasoning_off:
            budget = int(body.get("max_tokens") or 1000)
            reply = {"id": "mock", "object": "chat.completion", "model": body.get("model"),
                     "choices": [{"index": 0, "message": {"role": "assistant", "content": "",
                                                          "reasoning_content": "..."}, "finish_reason": "length"}],
                     "usage": {"prompt_tokens": len(user) // 4, "completion_tokens": budget,
                               "completion_tokens_details": {"reasoning_tokens": budget}}}
        else:
            content = answer(role, user)
            finish = "stop"
            if (SCENARIO == "truncated" and role == "analyst") or (SCENARIO == "review_cut" and role == "reviewer"):
                content, finish = content[: len(content) // 2], "length"
            reply = {"id": "mock", "object": "chat.completion", "model": body.get("model"),
                     "choices": [{"index": 0, "message": {"role": "assistant", "content": content}, "finish_reason": finish}],
                     "usage": {"prompt_tokens": len(user) // 4, "completion_tokens": len(content) // 4}}
        data = json.dumps(reply, ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    port = int(sys.argv[1])
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
