"use strict";
function evidence(ids, sources) {
  if (!ids || !ids.length) return el("span", { class: "ev unsupported" }, "[нет источника]");
  return el("span", { class: "ev" }, "[", ...ids.flatMap((id, i) => {
    const s = sources[id]; const url = s && safeUrl(s.url);
    const link = url ? el("a", { href: url, target: "_blank", rel: "noopener noreferrer", title: s.title || "" }, id) : el("button", {class:"evidence-link", title: s ? (s.title || s.document || "") : "Источник не найден", onclick:() => { const details = $("sources-details"); if (details) {details.open=true; $("source-" + id)?.scrollIntoView({block:"center"});} } }, id);
    return i ? [", ", link] : [link];
  }), "]");
}
function claims(title, list, sources, kind) {
  if (!list || !list.length) return null;
  return el("div", { class: "block " + (kind || "") }, kind === "ai" ? el("span", { class: "label ai" }, "Вывод AI") : null,
    el("h3", { style: "margin-top:0" }, title),
    el("ul", {}, list.map((c) => el("li", {}, c.text, c.supported_by_cited_source === false ? el("span",{class:"ev unsupported"}," · требует проверки") : null, c.horizon ? el("span", { class: "ev" }, " · " + c.horizon) : null, c.uses_bank_material ? el("span", { class: "ev bank-basis" }, " · по материалам Альфа-Банка") : null, evidence(c.evidence_ids, sources)))));
}

function renderInsight(d) {
  const sources = Object.fromEntries(d.sources.map((s) => [s.id, s]));
  const i = d.interpretation; const box = $("insight");
  const method = d.generation.method === "allowlisted_llm" ? `AI-агент (${d.generation.models.draft})` : "правила без модели";
  replaceContent(box,
    el("div", { class: "small muted" }, `Разбор для роли «${d.role.title}» · ${method}${d.cached ? " · из кэша" : ""}`),
    el("h2", { style: "margin-top:6px" }, d.article.title || "Новость"),
    el("div", { class: "block" }, el("span", { class: "label fact" }, "Что произошло"), el("div", {}, d.facts.what_happened, evidence(d.facts.evidence_ids, sources))),
    el("div", { class: "block ai" }, el("span", { class: "label ai" }, "Почему это важно вам"), el("div", {}, i.why_it_matters, evidence(i.why_it_matters_evidence_ids, sources)),
      el("div", { class: "small muted", style: "margin-top:6px" }, "Признаки компании: " + d.relevance.reasons.join("; "))),
    claims("Возможные последствия", i.potential_effects, sources, "ai"),
    claims("Риски", i.risks, sources, "ai"),
    claims("Возможности", i.opportunities, sources, "ai"),
    claims("Что можно сделать", i.possible_actions, sources, "ai"),
    i.bank_position && (i.bank_position.evidence_ids || []).some(id => id.startsWith('A') && sources[id]) ? el("div", { class: "block bank" }, el("span", { class: "label bank" }, "Позиция Альфа-Банка"), el("div", {}, i.bank_position.text, evidence(i.bank_position.evidence_ids, sources))) :
      el("div", { class: "block bank" }, el("span", { class: "label bank" }, "Позиция Альфа-Банка"), (i.bank_materials_used || []).length ?
        el("div", { class: "muted" }, "Прямой оценки этого события в материалах банка нет. Выводы с пометкой «по материалам Альфа-Банка» опираются на его аналитику: ",
          ...(i.bank_materials_used).flatMap((m, k) => [k ? "; " : "", `«${m.title}»` + (m.page ? `, стр. ${m.page}` : ""), evidence([m.id], sources)])) :
        el("div", { class: "muted" }, "В ответе нет позиции, подтверждённой ссылкой на материал банка. Связанные материалы можно посмотреть в источниках.")),
    ...(d.offers || []).map((o) => el("div", { class: "block" }, el("span", { class: "label ad" }, o.label + " · реклама"), el("div", {}, el("b", {}, o.product + ". "), o.text), el("div", { class: "small muted" }, "Почему предлагаем: " + o.reason))),
    i.uncertainties.length ? el("details", {}, el("summary", {}, `Неопределённости (${i.uncertainties.length})`), el("ul", {}, i.uncertainties.map((u) => el("li", { class: "small" }, u)))) : null,
    el("details", {id:"sources-details"}, el("summary", {}, `Источники (${d.sources.length})`), el("ul", {}, d.sources.map((s) => {
      const url = safeUrl(s.url);
      const title = s.title || `${s.document}, стр. ${s.page}`;
      return el("li", { class: "small", id:"source-" + s.id }, el("b", {}, s.id + " "), url ? el("a", { href: url, target: "_blank", rel: "noopener noreferrer" }, title) : title, el("span", { class: "muted" }, " · " + s.type));
    }))),
    el("details", {}, el("summary", {}, `Как агент пришёл к выводу (${d.agent_trace.length} шагов, подтверждено ${d.quality_check.claims_supported} из ${d.quality_check.claims_total} утверждений)`),
      ...d.agent_trace.map((t) => el("div", { class: "trace" }, el("b", { class: "status-" + t.status }, `${t.step} · ${t.status}`), el("span", { class: "muted" }, ` · ${t.duration_ms} мс`), el("div", {}, t.decision)))),
    el("h3", {}, "Уточнить"),
    el("div", { class: "row" }, ...[["risks", "Подробнее о рисках"], ["counterparties", "Влияние на контрагентов"], ["treasury", "Взгляд казначея"], ["scenarios", "Сценарии"], ["sources", "Первоисточники"]].map(([lens, label]) => el("button", { class: "ghost", onclick: () => ask("", lens) }, label))),
    el("div", { class: "row", style: "margin-top:8px" }, el("input", { id: "question", "aria-label": "Вопрос по выбранной новости", maxlength:"2000", placeholder: "Свой вопрос по этой новости", style: "flex:1" }), el("button", { class: "red", onclick: () => ask($("question").value, "") }, "Спросить")),
    el("div", { id: "answers" }),
    el("div", { class: "row", style: "margin-top:14px" }, el("span", { class: "small muted" }, "Разбор полезен?"),
      el("button", { class: "ghost", onclick: (e) => feedback(true, e.target) }, "Да"), el("button", { class: "ghost", onclick: (e) => feedback(false, e.target) }, "Нет")));
}


const pendingQuestions = new Set();
async function ask(question,lens) {
  if (!state.insight || pendingQuestions.has(state.insight.id)) return;
  if (!lens && !question.trim()) { $('question')?.focus(); return; }
  const insight = state.insight;
  const out=$('answers');
  const node=el('div',{class:'answer',role:'status'},'Готовим ответ…'); out.prepend(node);
  pendingQuestions.add(insight.id);
  try {
    const answer=await api(`/insights/${encodeURIComponent(insight.id)}/questions`,{question,lens});
    const sources=Object.fromEntries(insight.sources.map(s=>[s.id,s]));
    node.replaceChildren(el('b',{},answer.question || question || 'Уточнение'),el('p',{},answer.answer),evidence(answer.evidence_ids,sources));
  } catch(error) {node.replaceChildren(el('p',{},'Не удалось получить ответ. '+error.message),el('button',{class:'ghost',onclick:()=>ask(question,lens)},'Повторить'));}
  finally {pendingQuestions.delete(insight.id);}
}
async function feedback(useful,button) {
  if (!state.insight) return;
  const id=state.insight.id;
  const parent=button.parentElement;
  if (parent.dataset.sending) return;
  let reason='';
  if (!useful) reason=prompt('Что можно улучшить? (необязательно)') || '';
  parent.dataset.sending='true';
  parent.querySelectorAll('button').forEach(b=>b.disabled=true);
  try {
    await api('/feedback',{insight_id:id,useful,reason});
    parent.replaceChildren(el('span',{class:'small muted',role:'status'},'Спасибо, оценка учтена'));
  } catch(error) {
    parent.querySelector('.feedback-error')?.remove();
    parent.append(el('p',{class:'feedback-error',role:'alert'},'Оценка не отправлена. '+error.message));
    parent.querySelectorAll('button').forEach(b=>b.disabled=false);
  } finally {delete parent.dataset.sending;}
}
