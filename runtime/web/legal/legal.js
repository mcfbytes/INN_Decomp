// Fills the legal pages' placeholders from ../site.json. A value that is not set shows as a visible
// "[operator: set <field> in site.json]" and a banner says the deployment is not fully set up.
'use strict';

(async () => {
  let cfg = {};
  try {
    const res = await fetch('../site.json', { cache: 'no-cache' });
    if (res.ok) cfg = await res.json();
  } catch { /* the placeholders say what is missing */ }

  const DEFAULTS = { termsVersion: '1', minimumAge: '18', auditRetentionDays: '90' };
  const pick = (path) => {
    const v = path.split('.').reduce((o, k) => (o && typeof o === 'object' ? o[k] : undefined), cfg);
    return typeof v === 'number' ? String(v) : typeof v === 'string' ? v.trim() : '';
  };

  const missing = new Set();
  for (const node of document.querySelectorAll('[data-f]')) {
    const field = node.dataset.f;
    let value = pick(field);
    if (!value && node.dataset.fallback) value = pick(node.dataset.fallback);
    if (!value && (node.dataset.default || DEFAULTS[field])) value = node.dataset.default || DEFAULTS[field];
    if (!value) {
      missing.add(field);
      node.textContent = `[operator: set ${field} in site.json]`;
      node.classList.add('missing');
      continue;
    }
    if (node.hasAttribute('data-mail')) {
      node.textContent = value;
      if (/^[^\s@<>"]+@[^\s@<>"]+$/.test(value)) node.href = `mailto:${value}`;
    } else node.textContent = value;
  }

  const name = pick('siteName');
  if (name) {
    document.getElementById('brand').textContent = name;
    document.title = document.title.replace(/ · Online world$/, ` · ${name}`);
  }

  if (missing.size) {
    const banner = document.getElementById('banner');
    banner.textContent = 'This deployment has not filled in its operator details. ';
    const list = document.createElement('ul');
    for (const f of missing) {
      const li = document.createElement('li');
      li.textContent = `${f} (site.json)`;
      list.append(li);
    }
    banner.append(list);
    banner.hidden = false;
  }
})();
