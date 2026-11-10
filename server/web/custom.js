/* LeafControl: дополнения к PWA — статистика, обслуживание, детали поездок/зарядок.
   Всё в стилях приложения: те же переменные, карточки и шрифты. */
(function () {
  'use strict';

  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const fmt = (v, d) => (v === null || v === undefined || !isFinite(v)) ? '—'
    : Number(v).toFixed(d === undefined ? 1 : d).replace('.', ',');
  const api = async (p, body, method) => {
    const opt = { credentials: 'same-origin' };
    if (body !== undefined) {
      opt.method = method || 'POST';
      opt.headers = { 'Content-Type': 'application/json' };
      opt.body = JSON.stringify(body);
    }
    const r = await fetch(p, opt);
    let j = null; try { j = await r.json(); } catch {}
    if (!r.ok) throw new Error((j && j.error) || ('ошибка ' + r.status));
    return j;
  };

  /* ---------- 1. убираем лишние пункты "Ещё" ---------- */
  function cleanMore() {
    $$('.more-item').forEach(el => {
      const t = el.textContent || '';
      if (t.includes('Для разработчика') || t.includes('Упрощённая страница')) el.remove();
    });
  }

  /* ---------- 2. вкладка "Статистика" ---------- */
  const TAB_SVG = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M4 20V10M10 20V4M16 20v-7M22 20H2"/></svg>';

  function addTab() {
    const bar = $('.tabbar');
    if (!bar || bar.querySelector('[data-screen="stats"]')) return;
    const more = bar.querySelector('[data-screen="more"]');
    const tab = document.createElement('div');
    tab.className = 'tab';
    tab.dataset.screen = 'stats';
    tab.innerHTML = TAB_SVG + '<span>Статистика</span>';
    tab.addEventListener('click', (e) => {
      e.stopPropagation();
      showStats();
    }, true);
    bar.insertBefore(tab, more || null);
  }

  function addScreen() {
    const screens = $('#screens');
    if (!screens || $('#screen-stats')) return;
    const sec = document.createElement('section');
    sec.className = 'screen';
    sec.id = 'screen-stats';
    sec.innerHTML = `
      <div class="card" style="margin-top:8px">
        <div class="card-header"><div class="accent-line"></div>
          <div style="font-size:16px">Статистика</div></div>
        <div id="st-period" style="display:flex;gap:6px;margin:4px 0 12px"></div>
        <div id="st-totals"></div>
      </div>
      <div class="card">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Экономия</div></div>
        <div id="st-savings"></div>
        <div id="st-settings" style="margin-top:10px"></div>
      </div>
      <div class="card">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">SOH, %</div></div>
        <div id="st-soh"></div>
      </div>
      <div class="card">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Пробег по дням</div></div>
        <div id="st-days"></div>
      </div>
      <div class="card">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Обслуживание</div></div>
        <div id="st-maint"></div>
        <div id="st-maint-form" style="margin-top:10px"></div>
      </div>`;
    const more = $('#screen-more');
    screens.insertBefore(sec, more || null);
  }

  let statsCache = null;
  let statsDays = 0;

  function showStats() {
    // скрыть остальные экраны и подсветить свою вкладку
    $$('.screen').forEach(s => s.classList.remove('active'));
    const sec = $('#screen-stats');
    if (sec) sec.classList.add('active');
    $$('.tab').forEach(t => t.classList.toggle('active', t.dataset.screen === 'stats'));
    const sc = $('#screens'); if (sc) sc.scrollTop = 0;
    loadStats();
  }

  async function loadStats() {
    try {
      const d = await api('/api/stats?device=' + (await currentDevice()) + '&days=' + statsDays);
      statsCache = d;
      renderStats(d);
    } catch (e) {
      const el = $('#st-totals');
      if (el) el.innerHTML = '<div style="color:var(--muted);font-size:13px">' + e.message + '</div>';
    }
  }

  async function currentDevice() {
    try {
      const j = await api('/api/status');
      return j && j.device ? encodeURIComponent(j.device) : '';
    } catch { return ''; }
  }

  function chart(el, series, opts) {
    // series: [{values:[], color, label}]
    if (!el) return;
    const w = 300, h = 140, pad = 26;
    const all = series.flatMap(s => s.values).filter(v => isFinite(v));
    el.innerHTML = '';
    if (!all.length) {
      el.innerHTML = '<div style="font-size:12px;color:var(--muted);padding:8px 0">нет данных</div>';
      return;
    }
    let min = Math.min(...all), max = Math.max(...all);
    if (max - min < (opts && opts.minSpan || 1)) { max += 0.5; min -= 0.5; }
    const span = max - min;
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', '0 0 ' + w + ' ' + h);
    svg.setAttribute('style', 'width:100%;height:140px;display:block');
    const grid = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    grid.setAttribute('d', `M${pad} ${pad} H${w - pad} M${pad} ${h - pad} H${w - pad}`);
    grid.setAttribute('stroke', 'var(--line)'); grid.setAttribute('fill', 'none');
    svg.appendChild(grid);
    series.forEach(s => {
      const vals = s.values;
      if (!vals.length) return;
      const pts = vals.map((v, i) => [
        pad + i * (w - pad * 2) / Math.max(1, vals.length - 1),
        h - pad - ((v - min) / span) * (h - pad * 2)
      ]);
      const p = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      p.setAttribute('d', pts.map((q, i) => (i ? 'L' : 'M') + q[0].toFixed(1) + ' ' + q[1].toFixed(1)).join(' '));
      p.setAttribute('fill', 'none'); p.setAttribute('stroke', s.color || 'var(--accent)');
      p.setAttribute('stroke-width', '2'); p.setAttribute('stroke-linejoin', 'round');
      svg.appendChild(p);
    });
    const t = (x, y, s2, anchor) => {
      const e = document.createElementNS('http://www.w3.org/2000/svg', 'text');
      e.setAttribute('x', x); e.setAttribute('y', y); e.setAttribute('font-size', '11');
      e.setAttribute('fill', 'var(--muted)'); e.setAttribute('text-anchor', anchor || 'start');
      e.textContent = s2; svg.appendChild(e);
    };
    t(pad, pad - 6, fmt(max, 1));
    t(pad, h - pad + 14, fmt(min, 1));
    if (opts && opts.label) t(w - pad, pad - 6, opts.label, 'end');
    el.appendChild(svg);
  }

  function renderStats(d) {
    // периоды
    const periods = [[0, 'Всё время'], [30, '30 дней'], [7, '7 дней']];
    $('#st-period').innerHTML = periods.map(([v, t]) =>
      `<button class="dur-btns" style="flex:0 0 auto;width:auto;padding:6px 10px;border-radius:14px;border:1px solid var(--line);background:${v === statsDays ? 'var(--accent)' : 'var(--surface)'};color:${v === statsDays ? '#fff' : 'var(--text)'}" data-days="${v}">${t}</button>`
    ).join('');
    $$('#st-period button').forEach(b => b.onclick = () => {
      statsDays = Number(b.dataset.days);
      loadStats();
    });

    // итоги
    const rows = [
      ['Поездок', d.trips.count],
      ['Пробег', fmt(d.trips.km, 1) + ' км'],
      ['Расход', fmt(d.trips.kwh, 2) + ' кВт·ч'],
      ['км на кВт·ч', fmt(d.consumption_km_per_kwh, 2)],
      ['Зарядок', d.charges.count],
      ['Заряжено', fmt(d.charges.kwh, 2) + ' кВт·ч'],
    ];
    $('#st-totals').innerHTML = `<div style="display:grid;grid-template-columns:1fr 1fr;gap:10px">` +
      rows.map(([k, v]) => `<div><div style="font-size:12px;color:var(--muted)">${k}</div><div style="font-size:18px">${v}</div></div>`).join('') + `</div>`;

    // экономия
    const c = d.cost || {};
    $('#st-savings').innerHTML = `
      <div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px">
        <div><div style="font-size:12px;color:var(--muted)">Электричество</div><div style="font-size:17px">${fmt(c.electric, 0)} ${c.currency || ''}</div></div>
        <div><div style="font-size:12px;color:var(--muted)">Бензин</div><div style="font-size:17px">${fmt(c.petrol, 0)} ${c.currency || ''}</div></div>
        <div><div style="font-size:12px;color:var(--muted)">Экономия</div><div style="font-size:17px;color:var(--accent)">${fmt(c.saved, 0)} ${c.currency || ''}</div></div>
      </div>`;

    // настройки цен
    const s = d.settings || {};
    $('#st-settings').innerHTML = `
      <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px">
        <label style="font-size:12px;color:var(--muted)">Тариф день, ₽/кВт·ч
          <input id="set-day" type="number" step="0.01" value="${s.tariff_day || ''}" style="width:100%;padding:9px;border-radius:8px;border:1px solid var(--line);background:var(--surface);color:var(--text)">
        </label>
        <label style="font-size:12px;color:var(--muted)">Тариф ночь, ₽/кВт·ч
          <input id="set-night" type="number" step="0.01" value="${s.tariff_night || ''}" style="width:100%;padding:9px;border-radius:8px;border:1px solid var(--line);background:var(--surface);color:var(--text)">
        </label>
        <label style="font-size:12px;color:var(--muted)">Бензин, ₽/л
          <input id="set-petrol" type="number" step="0.01" value="${s.petrol_price || ''}" style="width:100%;padding:9px;border-radius:8px;border:1px solid var(--line);background:var(--surface);color:var(--text)">
        </label>
        <label style="font-size:12px;color:var(--muted)">Расход бензина, л/100
          <input id="set-l100" type="number" step="0.1" value="${s.petrol_l100 || ''}" style="width:100%;padding:9px;border-radius:8px;border:1px solid var(--line);background:var(--surface);color:var(--text)">
        </label>
      </div>
      <button id="set-save" style="margin-top:10px;width:100%;padding:11px;border:0;border-radius:8px;background:var(--accent);color:#fff;font-weight:600">Сохранить цены</button>`;
    $('#set-save').onclick = async () => {
      try {
        await api('/api/settings', {
          tariff_day: $('#set-day').value,
          tariff_night: $('#set-night').value,
          petrol_price: $('#set-petrol').value,
          petrol_l100: $('#set-l100').value,
        });
        loadStats();
      } catch (e) { alert(e.message); }
    };

    // SOH
    const soh = (d.soh || []).map(x => x.soh);
    if (soh.length) {
      chart($('#st-soh'), [{ values: soh, color: 'var(--accent)' }], { label: 'SOH %', minSpan: 2 });
      const last = soh[soh.length - 1];
      $('#st-soh').insertAdjacentHTML('beforeend',
        `<div style="font-size:12px;color:var(--muted);margin-top:4px">Текущий SOH: ${fmt(last, 2)} %</div>`);
    } else {
      $('#st-soh').innerHTML = '<div style="font-size:12px;color:var(--muted)">Плата пока не передаёт SOH.</div>';
    }

    // пробег по дням
    const days = d.per_day || [];
    if (days.length) {
      const maxKm = Math.max(...days.map(x => x.km), 1);
      $('#st-days').innerHTML = days.slice(-14).map(x => `
        <div style="display:flex;align-items:center;gap:8px;margin:4px 0">
          <div style="width:74px;font-size:11px;color:var(--muted)">${x.date.slice(5)}</div>
          <div style="flex:1;height:10px;background:var(--line);border-radius:5px;overflow:hidden">
            <div style="height:100%;width:${Math.round(100 * x.km / maxKm)}%;background:var(--accent)"></div>
          </div>
          <div style="width:64px;text-align:right;font-size:12px">${fmt(x.km, 1)} км</div>
        </div>`).join('');
    } else {
      $('#st-days').innerHTML = '<div style="font-size:12px;color:var(--muted)">Нет поездок за период.</div>';
    }

    // обслуживание
    const items = d.maintenance || [];
    $('#st-maint').innerHTML = items.length
      ? items.map(m => `
        <div style="display:flex;align-items:center;gap:8px;padding:8px 0;border-bottom:1px solid var(--line)">
          <div style="flex:1;font-size:14px">${m.title}
            <div style="font-size:12px;color:var(--muted)">${m.due_km ? 'к ' + m.due_km + ' км' : ''}${m.due_date ? ' · ' + m.due_date : ''}${m.note ? ' · ' + m.note : ''}</div>
          </div>
          <button data-done="${m.id}" style="border:0;background:transparent;color:var(--accent);font-size:13px">готово</button>
        </div>`).join('')
      : '<div style="font-size:12px;color:var(--muted)">Пока пусто. Добавьте замену масла, фильтров, шин…</div>';
    $$('#st-maint [data-done]').forEach(b => b.onclick = async () => {
      await api('/api/maintenance/done', { id: Number(b.dataset.done) });
      loadStats();
    });
    $('#st-maint-form').innerHTML = `
      <input id="mt-title" placeholder="Что заменить/проверить" style="width:100%;padding:9px;border-radius:8px;border:1px solid var(--line);background:var(--surface);color:var(--text);margin-bottom:6px">
      <input id="mt-km" type="number" placeholder="К пробегу, км" style="width:100%;padding:9px;border-radius:8px;border:1px solid var(--line);background:var(--surface);color:var(--text);margin-bottom:6px">
      <button id="mt-add" style="width:100%;padding:11px;border:0;border-radius:8px;background:var(--surface-2);color:var(--text);font-weight:600">Добавить</button>`;
    $('#mt-add').onclick = async () => {
      const title = $('#mt-title').value.trim();
      if (!title) return;
      await api('/api/maintenance', { device: d.device, title, due_km: Number($('#mt-km').value) || null });
      loadStats();
    };
  }

  /* ---------- 3. детали поездок и зарядок ---------- */
  const store = { trips: [], charges: [] };

  const origFetch = window.fetch;
  window.fetch = async function (input, init) {
    const r = await origFetch.apply(this, arguments);
    try {
      const url = typeof input === 'string' ? input : (input && input.url) || '';
      if (url.indexOf('/api/trips') >= 0 && (!init || !init.method || init.method === 'GET')) {
        r.clone().json().then(j => { store.trips = Array.isArray(j) ? j : []; }).catch(() => {});
      } else if (url.indexOf('/api/charges') >= 0 && (!init || !init.method || init.method === 'GET')) {
        r.clone().json().then(j => { store.charges = Array.isArray(j) ? j : []; }).catch(() => {});
      }
    } catch (e) { /* не мешаем приложению */ }
    return r;
  };

  function rangeOf(item, kind) {
    const list = kind === 'trip' ? store.trips : store.charges;
    const items = $$('.trip-item', $(kind === 'trip' ? '#screen-trips' : '#screen-charges'));
    const idx = items.indexOf(item);
    if (idx >= 0 && list[idx]) return [list[idx].s, list[idx].e, list[idx]];
    return null;
  }

  async function expand(item, kind) {
    if (item.dataset.expanded === '1') {
      const n = item.querySelector('.lc-detail');
      if (n) n.remove();
      item.dataset.expanded = '0';
      return;
    }
    const r = rangeOf(item, kind);
    if (!r) return;
    const [s, e, rec] = r;
    const box = document.createElement('div');
    box.className = 'lc-detail';
    box.style.cssText = 'padding:10px 0 4px;border-top:1px solid var(--line);margin-top:10px';
    box.innerHTML = `
      <div style="display:flex;gap:6px;margin-bottom:8px;flex-wrap:wrap">
        <button data-k="kw" class="lc-btn" style="padding:6px 10px;border-radius:14px;border:1px solid var(--line);background:var(--accent);color:#fff;font-size:12px">Мощность, кВт</button>
        ${kind === 'trip'
          ? '<button data-k="spd" class="lc-btn" style="padding:6px 10px;border-radius:14px;border:1px solid var(--line);background:var(--surface);color:var(--text);font-size:12px">Скорость</button>'
          : '<button data-k="batt" class="lc-btn" style="padding:6px 10px;border-radius:14px;border:1px solid var(--line);background:var(--surface);color:var(--text);font-size:12px">Батарея, °C</button>'}
        <button data-k="soc" class="lc-btn" style="padding:6px 10px;border-radius:14px;border:1px solid var(--line);background:var(--surface);color:var(--text);font-size:12px">Заряд, %</button>
      </div>
      <div class="lc-chart"></div>`;
    item.appendChild(box);
    item.dataset.expanded = '1';

    let pts = [];
    try {
      pts = (await api(`/api/samples?device=&from=${s}&to=${e}`)).points || [];
    } catch { }

    const series = { kw: [], spd: [], batt: [], soc: [] };
    pts.forEach(p => {
      if (p.kw !== undefined && p.kw !== null) series.kw.push(Number(p.kw));
      if (p.spd !== undefined && p.spd !== null) series.spd.push(Number(p.spd));
      if (p.batt !== undefined && p.batt !== null) series.batt.push(Number(p.batt));
      if (p.soc !== undefined && p.soc !== null) series.soc.push(Number(p.soc));
    });

    const colors = { kw: 'var(--accent)', spd: '#34c759', batt: '#ff9500', soc: '#af52de' };
    const state = { kw: true, spd: kind === 'charge', batt: false, soc: true };

    function draw() {
      const sel = Object.keys(state).filter(k => state[k] && series[k].length);
      const el = $('.lc-chart', box);
      if (!sel.length) {
        el.innerHTML = '<div style="font-size:12px;color:var(--muted)">Нет данных за эту поездку.</div>';
        return;
      }
      chart(el, sel.map(k => ({ values: series[k], color: colors[k] })), { minSpan: 2 });
      el.insertAdjacentHTML('beforeend',
        '<div style="display:flex;gap:12px;font-size:11px;color:var(--muted);margin-top:4px">' +
        sel.map(k => `<span><i style="display:inline-block;width:8px;height:8px;border-radius:2px;background:${colors[k]};margin-right:4px"></i>${k === 'kw' ? 'кВт' : k === 'spd' ? 'км/ч' : k === 'batt' ? '°C' : '%'}</span>`).join('') +
        '</div>');
    }

    $$('.lc-btn', box).forEach(b => b.onclick = () => {
      const k = b.dataset.k;
      state[k] = !state[k];
      b.style.background = state[k] ? 'var(--accent)' : 'var(--surface)';
      b.style.color = state[k] ? '#fff' : 'var(--text)';
      draw();
    });
    draw();
  }

  function bindCards() {
    const trips = $('#screen-trips');
    const charges = $('#screen-charges');
    if (trips && !trips.dataset.lcBound) {
      trips.dataset.lcBound = '1';
      trips.addEventListener('click', (e) => {
        const it = e.target.closest && e.target.closest('.trip-item');
        if (it) expand(it, 'trip');
      });
    }
    if (charges && !charges.dataset.lcBound) {
      charges.dataset.lcBound = '1';
      charges.addEventListener('click', (e) => {
        const it = e.target.closest && e.target.closest('.trip-item');
        if (it) expand(it, 'charge');
      });
    }
  }

  /* ---------- 3. настройки платы (внутри приложения) ---------- */
  function addSettingsScreen() {
    const screens = $('#screens');
    if (!screens || $('#screen-settings')) return;
    const sec = document.createElement('section');
    sec.className = 'screen';
    sec.id = 'screen-settings';
    sec.innerHTML = `
      <div class="card" style="margin-top:8px">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Настройки платы</div></div>
        <div id="cfg-info" style="font-size:12px;color:var(--muted);margin-bottom:12px"></div>
        <div id="cfg-fields"></div>
        <button id="cfg-save" style="margin-top:12px;width:100%;padding:12px;border:0;border-radius:8px;background:var(--accent);color:#fff;font-weight:600">Применить на плате</button>
        <div id="cfg-hint" style="font-size:12px;color:var(--muted);margin-top:8px"></div>
      </div>
      <div class="card">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Название машины</div></div>
        <input id="cfg-name" placeholder="Мой Leaf" style="width:100%;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--surface);color:var(--text)">
        <button id="cfg-rename" style="margin-top:10px;width:100%;padding:12px;border:0;border-radius:8px;background:var(--surface-2);color:var(--text);font-weight:600">Сохранить название</button>
      </div>`;
    screens.insertBefore(sec, $('#screen-more') || null);
  }

  function showSettings() {
    $$('.screen').forEach(s => s.classList.remove('active'));
    const sec = $('#screen-settings');
    if (sec) sec.classList.add('active');
    $$('.tab').forEach(t => t.classList.remove('active'));
    const sc = $('#screens'); if (sc) sc.scrollTop = 0;
    loadConfig();
  }

  const TOGGLES = [
    ['locking', 'Блокировка кнопок', 'запрет команд, если выключено'],
    ['sleep', 'Глубокий сон', 'усыплять плату при простое'],
    ['uds', 'Расширенная диагностика', 'UDS-запросы VCM/BCM/ABS/TMI'],
    ['uds_tpms', 'TPMS', 'если установлен модуль давления'],
    ['modem', 'Модем', 'SIM800 / A7670'],
  ];
  const NUMBERS = [
    ['charge_limit', 'Лимит заряда, %', '0 = выключено, 70–95'],
    ['sleep_after', 'Усыплять через, ч', ''],
    ['wake_fast', 'Быстрое пробуждение, мин', ''],
    ['wake_slow', 'Медленное пробуждение, мин', ''],
  ];

  async function loadConfig() {
    const fields = $('#cfg-fields');
    if (!fields) return;
    let cfg = {}, meta = {};
    try {
      const j = await api('/api/config');
      cfg = j.config || {}; meta = j;
    } catch (e) {
      fields.innerHTML = '<div style="font-size:12px;color:var(--muted)">' + e.message + '</div>';
      return;
    }

    $('#cfg-info').textContent =
      'Логин: ' + (meta.login || '—') +
      ' · прошивка: ' + (meta.fw || '—') +
      (meta.serial ? ' · MAC: ' + meta.serial : '') +
      (meta.ts ? ' · получено ' + new Date(meta.ts * 1000).toLocaleString('ru-RU') : ' · плата ещё не присылала настройки');

    const inputStyle = 'width:100%;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--surface);color:var(--text)';

    fields.innerHTML =
      NUMBERS.map(([k, t, h]) => `
        <label style="display:block;font-size:12px;color:var(--muted);margin-bottom:10px">${t}
          <input data-num="${k}" type="number" value="${cfg[k] !== undefined ? cfg[k] : ''}" style="${inputStyle}">
          ${h ? `<span style="font-size:11px">${h}</span>` : ''}
        </label>`).join('') +
      '<label style="display:block;font-size:12px;color:var(--muted);margin-bottom:10px">APN модема' +
      `<input data-text="apn" value="${(cfg.apn || '').replace(/"/g, '&quot;')}" style="${inputStyle}"></label>` +
      TOGGLES.map(([k, t, h]) => `
        <label style="display:flex;align-items:center;gap:10px;padding:8px 0;border-top:1px solid var(--line)">
          <input data-bool="${k}" type="checkbox" ${cfg[k] ? 'checked' : ''} style="width:18px;height:18px">
          <span style="font-size:14px">${t}${h ? `<span style="display:block;font-size:11px;color:var(--muted)">${h}</span>` : ''}</span>
        </label>`).join('');

    $('#cfg-name').value = meta.login ? '' : '';
    try {
      const st = await api('/api/status');
      if (st && st.device_name) $('#cfg-name').value = st.device_name;
    } catch {}

    $('#cfg-save').onclick = async () => {
      const hint = $('#cfg-hint');
      hint.textContent = 'Отправляю…';
      const jobs = [];
      $$('#cfg-fields [data-num]').forEach(el => {
        if (el.value !== '') jobs.push([el.dataset.num, Number(el.value)]);
      });
      $$('#cfg-fields [data-bool]').forEach(el => jobs.push([el.dataset.bool, el.checked ? 1 : 0]));
      $$('#cfg-fields [data-text]').forEach(el => {
        if (el.value) jobs.push([el.dataset.text, el.value]);
      });
      let ok = 0;
      for (const [key, value] of jobs) {
        try { await api('/api/config', { key, value }); ok++; } catch {}
      }
      hint.textContent = 'Отправлено параметров: ' + ok + '. Плата применит их в течение ' + 10 + ' с.';
      setTimeout(loadConfig, 5000);
    };

    $('#cfg-rename').onclick = async () => {
      const name = $('#cfg-name').value.trim();
      if (!name) return;
      try {
        const st = await api('/api/status');
        const devs = await api('/api/devices');
        const d = (devs.devices || []).find(x => x.login === st.device);
        if (!d) throw new Error('устройство не найдено');
        await api('/api/devices/' + d.id, { name }, 'PUT');
        toast('Название сохранено');
      } catch (e) { alert(e.message); }
    };
  }

  function interceptMore() {
    $$('.more-item').forEach(el => {
      if (el.dataset.lcHooked) return;
      const t = el.textContent || '';
      if (t.includes('Настройки платы')) {
        el.dataset.lcHooked = '1';
        el.addEventListener('click', (e) => {
          e.preventDefault();
          e.stopImmediatePropagation();
          showSettings();
        }, true);
      }
    });
  }

  function toast(text) {
    let el = document.getElementById('lc-toast');
    if (!el) {
      el = document.createElement('div');
      el.id = 'lc-toast';
      el.style.cssText = 'position:fixed;left:50%;bottom:24px;transform:translateX(-50%);' +
        'background:rgba(28,28,30,.94);color:#fff;padding:10px 16px;border-radius:12px;' +
        'font-size:14px;z-index:999;opacity:0;transition:.25s';
      document.body.appendChild(el);
    }
    el.textContent = text;
    el.style.opacity = '1';
    clearTimeout(el._t);
    el._t = setTimeout(() => { el.style.opacity = '0'; }, 2600);
  }

  /* ---------- запуск ---------- */
  function init() {
    cleanMore();
    addScreen();
    addTab();
    addSettingsScreen();
    interceptMore();
    bindCards();
    const obs = new MutationObserver(() => { cleanMore(); interceptMore(); bindCards(); });
    obs.observe(document.body, { childList: true, subtree: true });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
