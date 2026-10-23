'use strict';
const API = '/api/v1';
const $ = id => document.getElementById(id);
const {safeUrl} = AlfaCore;
const state = {role:'cfo', client:null, company:null, newsId:null, insight:null, items:[], page:0, focused:false, clientOffset:0, clientTotal:0};
const requests = new Map();
let feedVersion = 0, insightVersion = 0, clientsVersion = 0;
const productNames = {loans:'кредиты',leasing:'лизинг',deposits:'депозиты',bank_guarantees:'гарантии',factoring:'факторинг',salary_project:'зарплатный проект',rko:'РКО'};
function el(tag, attrs, ...children) {
  const node = document.createElement(tag);
  for (const [key,value] of Object.entries(attrs || {})) {
    if (key === 'class') node.className = value;
    else if (key.startsWith('on')) node.addEventListener(key.slice(2),value);
    else if (value !== null && value !== undefined) node.setAttribute(key,value);
  }
  for (const child of children.flat()) if (child !== null && child !== undefined) node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  return node;
}
async function api(path, body, key) {
  const controller = new AbortController();
  if (key) { requests.get(key)?.abort(); requests.set(key,controller); }
  const timer = setTimeout(() => controller.abort('timeout'), body ? 90000 : 20000);
  try {
    const response = await fetch(API + path, {signal:controller.signal, ...(body ? {method:'POST', headers:{'Content-Type':'application/json'},body:JSON.stringify(body)} : {})});
    const data = await response.json().catch(() => {throw new Error('Сервер вернул неожиданный ответ. Проверьте доступность API.');});
    if (!response.ok) throw new Error(data.message || 'Ошибка сервера: ' + response.status);
    return data;
  } catch(error) {
    if (controller.signal.reason === 'timeout') throw new Error('Сервер не ответил вовремя. Попробуйте ещё раз.');
    throw error;
  } finally { clearTimeout(timer); if (key && requests.get(key) === controller) requests.delete(key); }
}
function date(value) { const d = new Date(value); return value && !Number.isNaN(d.getTime()) ? d.toLocaleDateString('ru-RU',{day:'numeric',month:'short'}) : 'Дата не указана'; }
function money(value) { return Number.isFinite(value) ? (value / 1e9).toLocaleString('ru-RU',{maximumFractionDigits:1}) + ' млрд ₽' : 'Выручка не указана'; }
function currentNews() {return state.items.find(n => n.news_id === state.newsId);}
function replaceContent(box,...children) {box.replaceChildren(...children.filter(child => child !== null && child !== undefined));}
function status(box,text,retry) {replaceContent(box,el('p',{},text), retry ? el('button',{class:'subtle',onclick:retry},'Повторить') : null); box.hidden = false;}
function route() {
  const view = location.hash === '#scenarios' ? 'scenarios' : location.hash === '#map' ? 'map' : 'home';
  $('home-panel').hidden = view !== 'home'; $('map-panel').hidden = view !== 'map'; $('scenario-panel').hidden = view !== 'scenarios';
  if (view === 'home') $('insight-section').hidden = true;
  else if (state.insight) $('insight-section').hidden = false;
  for (const [id,active] of [['tab-home',view === 'home'],['tab-map',view === 'map'],['tab-scenarios',view === 'scenarios']]) {
    if (active) $(id).setAttribute('aria-current','page'); else $(id).removeAttribute('aria-current');
  }
  document.title = ({home:'Главная',map:'Новости для бизнеса',scenarios:'Сценарии'})[view] + ' · Альфа-Бизнес';
}
function updateHome() {
  const top = state.items[0];
  const role = $('role').selectedOptions[0]?.textContent || '';
  $('home-role').textContent = role ? 'Персонально для роли: ' + role : 'Персонально для вашей роли';
  $('fan-role').textContent = ({owner:'Owner',ceo:'CEO',cfo:'CFO',treasurer:'Treasury',accountant:'Accounting'})[state.role] || state.role.toUpperCase();
  $('fan-count').textContent = state.items.length ? String(state.items.length) : '—';
  $('fan-score').textContent = top ? Math.round((top.relevance?.score || 0) * 100) + '%' : '—';
  $('fan-top').textContent = top ? top.title : 'Выберите компанию, чтобы получить ленту';
  $('home-top-title').textContent = top ? top.title : 'Выберите компанию, чтобы получить персональную ленту.';
  $('home-top-label').textContent = top ? 'Главное событие · ' + (top.relevance?.primary_category_label || '') : 'Главное событие для вас';
}
function clearInsight() {
  insightVersion++; requests.get('insight')?.abort();
  state.insight = null; $('insight-section').hidden = true; $('insight').replaceChildren();
  $('map-open').disabled = false; $('map-open').textContent = 'Получить разбор';
}
function updateScenarioContext() {
  const n = currentNews();
  $('scenario-context').textContent = n ? n.title : 'Выберите событие на карте, чтобы связать его с расчётом.';
  $('scenario-analysis').disabled = !n;
}
async function bootstrap() {
  $('service-status').hidden = true;
  const [roles,facets] = await Promise.allSettled([api('/roles',null,'roles'),api('/clients/facets',null,'facets')]);
  if (roles.status === 'rejected' || !roles.value?.items?.length) {
    status($('service-status'),'Не удалось загрузить роли. ' + (roles.reason?.message || 'Нет доступных ролей.'),bootstrap);
    status($('map-status'),'Для загрузки аналитики требуется подключение к API.'); return;
  }
  $('role').replaceChildren(...roles.value.items.map(r => el('option',{value:r.id},r.title)));
  if (!roles.value.items.some(r => r.id === state.role)) state.role = roles.value.items[0].id;
  $('role').value = state.role; $('role').disabled = false;
  $('role').onchange = () => {state.role = $('role').value; updateRole(); loadFeed();};
  function updateRole() {$('role-focus').textContent = roles.value.items.find(r => r.id === state.role)?.focus || '';}
  updateRole();
  $('client-industry').replaceChildren(el('option',{value:''},'Все отрасли'));
  if (facets.status === 'fulfilled') for (const f of facets.value.industries || []) $('client-industry').append(el('option',{value:f.name},f.name + ' (' + f.count + ')'));
  await loadClients(true);
}
async function loadClients(autoSelect = false) {
  const version = ++clientsVersion;
  $('clients-prev').disabled = $('clients-next').disabled = true;
  status($('clients'),'Загружаем компании…');
  const q = new URLSearchParams({limit:'12',offset:String(state.clientOffset),q:$('client-q').value.trim(),industry:$('client-industry').value});
  try {
    const data = await api('/clients?' + q,null,'clients');
    if (version !== clientsVersion) return;
    state.clientTotal = data.total || 0;
    $('clients').replaceChildren();
    if (!data.items?.length) status($('clients'),'Компании не найдены. Измените поиск или отрасль.');
    for (const c of data.items || []) $('clients').append(el('button',{class:'client' + (state.client === c.id ? ' on' : ''),onclick:() => chooseCompany(c)},el('strong',{},c.id),el('span',{class:'muted small'},money(c.revenue_rub)),el('p',{},c.industry || 'Отрасль не указана'),el('small',{},c.region || c.city || 'Регион не указан')));
    $('clients-meta').textContent = data.items?.length ? `${state.clientOffset + 1}–${state.clientOffset + data.items.length} из ${data.total}` : 'Нет результатов';
    $('clients-prev').disabled = state.clientOffset === 0;
    $('clients-next').disabled = state.clientOffset + 12 >= state.clientTotal;
    if (autoSelect && !state.client && data.items?.length) chooseCompany(data.items[0]);
    else if (autoSelect && !state.client) status($('map-status'),'В выборке нет компаний. Попробуйте изменить поиск.',() => $('company-dialog').showModal());
  } catch(error) {
    if (version !== clientsVersion || error.name === 'AbortError') return;
    status($('clients'),'Не удалось загрузить компании. ' + error.message,() => loadClients(autoSelect));
    if (!state.client) status($('map-status'),'Не удалось загрузить компании. ' + error.message,() => loadClients(true));
  }
}
function chooseCompany(company) {
  state.client = company.id; state.company = company;
  $('company-name').textContent = company.id; $('company-region').textContent = company.region || company.city || 'Регион не указан';
  $('company-dialog').close(); loadFeed();
}
async function loadFeed() {
  if (!state.client) { $('company-dialog').showModal(); return; }
  const version = ++feedVersion;
  clearInsight(); state.items = []; state.newsId = null; state.page = 0; updateScenarioContext(); updateHome();
  $('map-workspace').hidden = true; $('feed-pager').hidden = true; $('feed-gaps').replaceChildren();
  status($('map-status'),'Подбираем события для компании и вашей роли…');
  $('refresh').disabled = true;
  try {
    const data = await api(`/companies/${encodeURIComponent(state.client)}/feed?role=${encodeURIComponent(state.role)}&limit=30`,null,'feed');
    if (version !== feedVersion) return;
    state.items = (data.items || []).filter(n => typeof n.news_id === 'string');
    $('feed-meta').textContent = `Событий: ${state.items.length} · ${data.role?.title || $('role').selectedOptions[0]?.textContent || ''}`;
    $('feed-gaps').replaceChildren(...(data.data_gaps || []).map(g => el('p',{},g)));
    if (!state.items.length) {status($('map-status'),'По этому профилю пока нет событий. Можно сменить компанию или обновить ленту.',loadFeed);return;}
    state.newsId = state.items[0].news_id; updateHome();
    $('map-status').hidden = true; $('map-workspace').hidden = false; $('feed-pager').hidden = false;
    renderMap();
  } catch(error) {
    if (version !== feedVersion || error.name === 'AbortError') return;
    status($('map-status'),'Не удалось загрузить ленту. ' + error.message,loadFeed);
  } finally {if (version === feedVersion) $('refresh').disabled = false;}
}
function selectNews(news) {if (state.newsId !== news.news_id) clearInsight(); state.newsId = news.news_id; renderMap();}
function renderMap() {
  const page = AlfaCore.pageItems(state.items,state.page);
  const selected = page.findIndex(n => n.news_id === state.newsId);
  const positions = ['one','two','three'];
  $('event-nodes').replaceChildren(); $('effects').replaceChildren();
  page.forEach((n,index) => {
    const active = index === selected;
    const node = el('button',{class:`graph-node event-node event-${positions[index]}` + (active ? ' selected' : ''),'aria-pressed':String(active),title:n.title,onclick:() => selectNews(n)},el('span',{class:'node-tag'},n.source || 'Источник'),el('strong',{},n.title),el('small',{},date(n.published)));
    node.style.opacity = state.focused && !active ? '.25' : '1'; $('event-nodes').append(node);
    const effect = el('div',{class:`effect effect-${positions[index]}`},el('span',{class:'effect-dot'}),el('b',{},n.relevance?.primary_category_label || n.category_labels?.[0] || 'Бизнес-контекст'),el('small',{},'Релевантность ' + Math.round((n.relevance?.score || 0)*100) + '%'));
    effect.style.opacity = state.focused && !active ? '.25' : '1'; $('effects').append(effect);
  });
  document.querySelectorAll('.path-group').forEach((path,index) => {path.style.display = index < page.length ? '' : 'none'; path.style.opacity = state.focused && index !== selected ? '.15' : '1'; path.style.stroke = index === selected ? '#ed867e' : '#b3bfce';});
  document.querySelectorAll('.flow').forEach(flow => {
    flow.style.display = selected < 0 ? 'none' : '';
    if (selected >= 0) flow.setAttribute('d',document.querySelector('.path-' + selected).getAttribute('d'));
  });
  $('graph-company').textContent = state.client;
  $('graph-region').textContent = state.company?.region || 'Ваша компания';
  const products = (state.company?.products || []).slice(0,3).map(p => productNames[p] || p);
  $('graph-products').textContent = [state.company?.foreign_trade ? 'ВЭД' : '',...products].filter(Boolean).join(' · ') || 'Профиль компании';
  $('feed-prev').disabled = state.page === 0; $('feed-next').disabled = (state.page+1)*3 >= state.items.length;
  $('feed-page').textContent = `${state.page+1} / ${Math.ceil(state.items.length/3)}`;
  const n = currentNews(); if (!n) return;
  $('node-title').textContent = n.title; $('node-copy').textContent = n.why_for_you || 'Причина релевантности не указана.';
  $('node-index').textContent = `${state.items.indexOf(n)+1} / ${state.items.length}`;
  $('news-source').textContent = n.source || 'Источник не указан';
  $('news-reasons').replaceChildren(el('details',{},el('summary',{},'Все признаки релевантности'),el('ul',{},(n.relevance?.reasons || []).map(reason => el('li',{},reason)))));
  $('bank-hint').hidden = !n.bank_hint;
  if (n.bank_hint) $('bank-hint').replaceChildren(el('strong',{},'Материал Альфа-Банка'),el('p',{},n.bank_hint.title),el('small',{},'Связанный контекст, не позиция банка по событию'));
  const url = safeUrl(n.url); $('news-original').hidden = !url;
  if (url) $('news-original').href = url; else $('news-original').removeAttribute('href');
  $('linked-scenario').hidden = !AlfaCore.isCurrency(n);
  updateScenarioContext();
}
async function loadInsight(newsId = state.newsId) {
  if (!newsId || !state.client) return;
  $('insight-section').hidden = false;
  $('insight-section').scrollIntoView({behavior:matchMedia('(prefers-reduced-motion: reduce)').matches ? 'instant' : 'smooth',block:'start'});
  if (requests.has('insight')) return;
  if (state.insight && state.insight.article && state.newsId === newsId) return;
  const version = ++insightVersion;
  const company = state.client, role = state.role;
  $('map-open').disabled = true; $('map-open').textContent = 'Готовим разбор…';
  status($('insight'),'Аналитик проверяет событие, профиль компании и материалы банка…');
  try {
    const result = await api('/insights',{company_id:company,news_id:newsId,role},'insight');
    if (version !== insightVersion || state.client !== company || state.role !== role || state.newsId !== newsId) return;
    renderInsight(result); state.insight = result;
  } catch(error) {
    if (version !== insightVersion || error.name === 'AbortError') return;
    status($('insight'),'Не удалось подготовить разбор. ' + error.message,() => loadInsight(newsId));
  } finally {if (version === insightVersion) {$('map-open').disabled = false; $('map-open').textContent = state.insight ? 'Открыть разбор' : 'Получить разбор';}}
}
function updateCalculation() {
  const amount = Number($('amount').value), base = Number($('base-rate').value), rate = Number($('scenario-rate').value);
  const result = AlfaCore.calculate(amount,base,rate);
  for (const [id,value,max,min] of [['amount',amount,1e12,1],['base-rate',base,100000,.01],['scenario-rate',rate,100000,.01]]) $(id).setAttribute('aria-invalid',String(!Number.isFinite(value) || value < min || value > max));
  $('amount-currency').textContent = $('currency').value;
  if (!result) {
    $('calculation-note').textContent = 'Введите сумму от 1 до 1 трлн и курсы от 0,01 до 100 000 ₽.';
    for (const id of ['base-value','current-value','delta-value','delta-percent','base-label','bar-rate']) $(id).textContent = '—';
    $('base-bar').style.height = $('scenario-bar').style.height = '0px';
    return;
  }
  const fmt = (v,d=2) => v.toLocaleString('ru-RU',{maximumFractionDigits:d});
  const max = Math.max(result.before,result.after);
  const divisor = max >= 1e12 ? 1e12 : max >= 1e9 ? 1e9 : max >= 1e6 ? 1e6 : max >= 1e3 ? 1e3 : 1;
  const unit = divisor === 1e12 ? 'трлн ₽' : divisor === 1e9 ? 'млрд ₽' : divisor === 1e6 ? 'млн ₽' : divisor === 1e3 ? 'тыс. ₽' : '₽';
  $('base-value').textContent = fmt(result.before/divisor,3); $('current-value').textContent = fmt(result.after/divisor,3);
  $('base-unit').textContent = $('current-unit').textContent = unit;
  for (const id of ['base-value','current-value']) $(id).style.fontSize = $(id).textContent.length > 6 ? '26px' : '';
  $('delta-value').textContent = (result.delta > 0 ? '+' : '') + fmt(result.delta/divisor,3) + ' ' + unit;
  $('delta-percent').textContent = (result.percent > 0 ? '+' : '') + fmt(result.percent) + '%';
  $('base-label').textContent = fmt(base) + ' ₽'; $('bar-rate').textContent = fmt(rate) + ' ₽';
  $('base-bar').style.height = result.baseHeight + 'px'; $('scenario-bar').style.height = result.rateHeight + 'px';
  $('rate').min = Math.max(.01,Math.min(base*.5,rate)); $('rate').max = Math.min(100000,Math.max(base*1.5,rate));
  $('rate').value = rate;
  $('range-min').textContent = fmt(Number($('rate').min)) + ' ₽'; $('range-max').textContent = fmt(Number($('rate').max)) + ' ₽';
  $('calculation-note').textContent = 'Комиссии и хеджирование не учтены. Значения не являются прогнозом.';
}
function init() {
  window.addEventListener('hashchange',route); route();
  $('home-date').textContent = new Date().toLocaleDateString('ru-RU',{weekday:'long',day:'numeric',month:'long'});
  $('choose-company').onclick = () => {$('company-dialog').showModal(); loadClients();};
  $('close-company').onclick = () => $('company-dialog').close();
  $('company-search-form').onsubmit = e => {e.preventDefault();state.clientOffset=0;loadClients();};
  $('client-industry').onchange = () => {state.clientOffset=0;loadClients();};
  $('clients-prev').onclick = () => {state.clientOffset=Math.max(0,state.clientOffset-12);loadClients();};
  $('clients-next').onclick = () => {state.clientOffset+=12;loadClients();};
  $('refresh').onclick = () => state.client ? loadFeed() : bootstrap();
  $('feed-prev').onclick = () => {state.page--;selectNews(state.items[state.page*3]);};
  $('feed-next').onclick = () => {state.page++;selectNews(state.items[state.page*3]);};
  for (const [id,focused] of [['mode-all',false],['mode-focus',true]]) $(id).onclick = () => {state.focused=focused; $('mode-all').setAttribute('aria-pressed',String(!focused)); $('mode-focus').setAttribute('aria-pressed',String(focused)); $('mode-all').classList.toggle('active',!focused); $('mode-focus').classList.toggle('active',focused);renderMap();};
  $('map-open').onclick = () => loadInsight(); $('scenario-analysis').onclick = () => loadInsight();
  $('close-insight').onclick = () => {$('insight-section').hidden = true; (location.hash === '#scenarios' ? $('scenario-analysis') : $('map-open')).focus();};
  $('motion-toggle').onclick = () => {const off=document.body.classList.toggle('no-motion'); $('motion-toggle').setAttribute('aria-pressed',String(off)); $('motion-toggle').textContent=off?'Включить движение':'Остановить движение';};
  for (const id of ['amount','base-rate','scenario-rate','currency']) $(id).addEventListener('input',updateCalculation);
  $('rate').oninput = () => {$('scenario-rate').value=$('rate').value;updateCalculation();};
  $('reset-scenario').onclick = () => {$('amount').value=1000000;$('base-rate').value=80;$('scenario-rate').value=88;$('currency').value='USD';updateCalculation();};
  updateCalculation(); bootstrap();
}
