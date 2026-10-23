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
  const core = {calculate, safeUrl, pageItems, isCurrency};
  if (typeof module !== 'undefined' && module.exports) module.exports = core;
  else root.AlfaCore = core;
})(typeof globalThis !== 'undefined' ? globalThis : this);
