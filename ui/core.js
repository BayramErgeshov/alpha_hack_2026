(function(root) {
  'use strict';
  function calculate(amount, base, rate) {
    if (![amount, base, rate].every(Number.isFinite) || amount < 1 || amount > 1e12 || base < .01 || rate < .01 || base > 100000 || rate > 100000) return null;
    const before = amount * base, after = amount * rate;
    const max = Math.max(before, after);
    return {before, after, delta: after - before, percent: (rate / base - 1) * 100, baseHeight: before / max * 190, rateHeight: after / max * 190};
  }
  function safeUrl(url) { try { const parsed = new URL(url); return ['https:', 'http:'].includes(parsed.protocol) ? parsed.href : null; } catch { return null; } }
  function pageItems(items, page, size = 3) { return items.slice(page * size, (page + 1) * size); }
  function isCurrency(news) { return Boolean(news && (news.categories || []).includes('currency')); }
  function sourcesWord(n) {
    const d = n % 10, h = n % 100;
    if (d === 1 && h !== 11) return 'источник';
    if (d >= 2 && d <= 4 && (h < 12 || h > 14)) return 'источника';
    return 'источников';
  }
  // "Ведомости · ещё 2 источника: ТАСС, Коммерсантъ" when other outlets reported the same event.
  function sourceLine(news) {
    const main = (news && news.source) || 'Источник не указан';
    const others = (news && news.also_reported) || [];
    if (!others.length) return main;
    const names = [...new Set(others.map(o => o.source).filter(Boolean))];
    return `${main} · ещё ${others.length} ${sourcesWord(others.length)}` + (names.length ? `: ${names.join(', ')}` : '');
  }
  const core = {calculate, safeUrl, pageItems, isCurrency, sourceLine};
  if (typeof module !== 'undefined' && module.exports) module.exports = core;
  else root.AlfaCore = core;
})(typeof globalThis !== 'undefined' ? globalThis : this);
