/* LeafControl: дополнения к PWA — статистика, обслуживание, детали поездок/зарядок.
   Всё в стилях приложения: те же переменные, карточки и шрифты. */
(function () {
  'use strict';

  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const fmt = (v, d) => (v === null || v === undefined || !isFinite(v)) ? '—'
    : Number(v).toFixed(d === undefined ? 1 : d).replace('.', ',');
  // Chrome/Edge предлагают установку PWA — придерживаем событие до нажатия кнопки.
  let deferredInstall = null;
  window.addEventListener('beforeinstallprompt', (e) => {
    e.preventDefault();
    deferredInstall = e;
  });
  window.addEventListener('appinstalled', () => { deferredInstall = null; });

  const isStandalone = () =>
    window.matchMedia('(display-mode: standalone)').matches || window.navigator.standalone === true;

  async function installPwa() {
    if (isStandalone()) { toast('Уже открыто как приложение'); return; }
    if (deferredInstall) {
      try {
        deferredInstall.prompt();
        const res = await deferredInstall.userChoice;
        deferredInstall = null;
        if (res && res.outcome === 'accepted') return;
      } catch (e) { /* ниже подскажем вручную */ }
    }
    const iOS = /iPad|iPhone|iPod/.test(navigator.userAgent);
    alert(iOS
      ? 'В Safari нажмите «Поделиться» и выберите «На экран «Домой».'
      : 'В меню браузера (⋮) выберите «Установить приложение» или «Добавить на главный экран».');
  }

  const api = async (p, body, method) => {
    const opt = { credentials: 'same-origin' };
    // method можно передать отдельно, без тела (DELETE и подобное)
    opt.method = method || (body !== undefined ? 'POST' : 'GET');
    if (body !== undefined) {
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

  // Рисует график по точкам [[epoch, значение], ...] или просто [значение, ...].
  // Ширина берётся из карточки, линия сглаживается, по горизонтали — время.
  // Для мощности рисуется нулевая линия: выше — потребление, ниже — рекуперация
  // (зелёным).
  function chart(el, series, opts) {
    if (!el) return;
    opts = opts || {};
    const SVGNS = 'http://www.w3.org/2000/svg';
    const W = Math.max(240, Math.floor(el.clientWidth || (el.parentElement && el.parentElement.clientWidth) || 320));
    const H = opts.height || 300;
    const padL = 36, padR = 12, padT = 18, padB = 22;
    const plotW = W - padL - padR, plotH = H - padT - padB;

    const all = [];
    series.forEach(sr => {
      const raw = sr.points || (sr.values || []).map(v => [0, v]);
      sr._raw = raw.map(p => [Number(p[0]) || 0, Number(p[1])]).filter(p => isFinite(p[1]));
      // лёгкое сглаживание 1-2-1: убирает «пилу» телеметрии, форма остаётся
      const sm = sr._raw.map(p => [p[0], p[1]]);
      for (let i = 1; i < sm.length - 1; i++) {
        sm[i][1] = (sr._raw[i - 1][1] + 2 * sr._raw[i][1] + sr._raw[i + 1][1]) / 4;
      }
      sr._pts = sm;
      sr._pts.forEach(p => all.push(p[1]));
    });

    el.innerHTML = '';
    if (!all.length) {
      el.innerHTML = '<div style="font-size:12px;color:var(--muted);padding:8px 0">нет данных</div>';
      return;
    }

    let min = Math.min(...all), max = Math.max(...all);
    const step = opts.minSpan || 1;
    if (max - min < step) {
      const c = (max + min) / 2;
      min = c - step / 2;
      max = c + step / 2;
    }
    const span = (max - min) || 1;

    let t0 = opts.from, t1 = opts.to;
    if (!t0 || !t1) {
      const tsAll = series.flatMap(sr => (sr.points || []).map(p => Number(p[0]) || 0)).filter(t => t > 0);
      if (tsAll.length) { t0 = Math.min(...tsAll); t1 = Math.max(...tsAll); }
    }
    const hasTime = !!(t0 && t1 && t1 > t0);
    const longSpan = hasTime && (t1 - t0) > 86400;

    const xFor = (p, i, n) => {
      if (hasTime && p[0] > 0) {
        const f = Math.min(1, Math.max(0, (p[0] - t0) / (t1 - t0)));
        return padL + f * plotW;
      }
      return padL + (n > 1 ? i / (n - 1) : 0.5) * plotW;
    };
    const yFor = (v) => padT + plotH - ((v - min) / span) * plotH;

    const svg = document.createElementNS(SVGNS, 'svg');
    svg.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
    svg.setAttribute('width', W);
    svg.setAttribute('height', H);
    svg.setAttribute('style', 'display:block;width:100%;height:' + H + 'px');

    // горизонтальная и вертикальная сетка
    [0, 0.5, 1].forEach(f => {
      const line = document.createElementNS(SVGNS, 'path');
      line.setAttribute('d', `M${padL} ${(padT + f * plotH).toFixed(1)} H${W - padR}`);
      line.setAttribute('stroke', 'var(--line)');
      line.setAttribute('fill', 'none');
      svg.appendChild(line);
    });
    [0.25, 0.5, 0.75].forEach(f => {
      const line = document.createElementNS(SVGNS, 'path');
      line.setAttribute('d', `M${(padL + f * plotW).toFixed(1)} ${padT} V${padT + plotH}`);
      line.setAttribute('stroke', 'var(--line)');
      line.setAttribute('fill', 'none');
      line.setAttribute('opacity', '0.6');
      svg.appendChild(line);
    });

    // сглаженная линия: квадратичные сегменты через середины
    const smooth = (pts) => {
      if (!pts.length) return '';
      if (pts.length === 1) return `M${pts[0][0].toFixed(1)} ${pts[0][1].toFixed(1)}`;
      let d = `M${pts[0][0].toFixed(1)} ${pts[0][1].toFixed(1)}`;
      for (let i = 1; i < pts.length - 1; i++) {
        const mx = (pts[i][0] + pts[i + 1][0]) / 2;
        const my = (pts[i][1] + pts[i + 1][1]) / 2;
        d += ` Q${pts[i][0].toFixed(1)} ${pts[i][1].toFixed(1)} ${mx.toFixed(1)} ${my.toFixed(1)}`;
      }
      const last = pts[pts.length - 1];
      d += ` L${last[0].toFixed(1)} ${last[1].toFixed(1)}`;
      return d;
    };

    // нулевая линия и зелёная зона рекуперации (только если шкала через ноль)
    const hasZero = min < 0 && max > 0;
    let yZero = 0;
    if (hasZero) {
      yZero = yFor(0);
      const zl = document.createElementNS(SVGNS, 'path');
      zl.setAttribute('d', `M${padL} ${yZero.toFixed(1)} H${W - padR}`);
      zl.setAttribute('stroke', 'var(--muted)');
      zl.setAttribute('stroke-width', '1');
      zl.setAttribute('stroke-dasharray', '4 3');
      zl.setAttribute('fill', 'none');
      svg.appendChild(zl);
    }

    const defs = document.createElementNS(SVGNS, 'defs');
    const clipAbove = document.createElementNS(SVGNS, 'clipPath');
    clipAbove.setAttribute('id', 'clipAbove');
    const rectAbove = document.createElementNS(SVGNS, 'rect');
    rectAbove.setAttribute('x', padL); rectAbove.setAttribute('y', padT);
    rectAbove.setAttribute('width', plotW); rectAbove.setAttribute('height', Math.max(0, yZero - padT));
    clipAbove.appendChild(rectAbove);
    const clipBelow = document.createElementNS(SVGNS, 'clipPath');
    clipBelow.setAttribute('id', 'clipBelow');
    const rectBelow = document.createElementNS(SVGNS, 'rect');
    rectBelow.setAttribute('x', padL); rectBelow.setAttribute('y', yZero);
    rectBelow.setAttribute('width', plotW); rectBelow.setAttribute('height', Math.max(0, padT + plotH - yZero));
    clipBelow.appendChild(rectBelow);
    defs.appendChild(clipAbove);
    defs.appendChild(clipBelow);
    svg.appendChild(defs);

    series.forEach(sr => {
      const pts = sr._pts;
      if (!pts.length) return;
      const dst = smooth(pts.map((p, i) => [xFor(p, i, pts.length), yFor(p[1])]));
      const mk = (color, clip) => {
        const path = document.createElementNS(SVGNS, 'path');
        path.setAttribute('d', dst);
        path.setAttribute('fill', 'none');
        path.setAttribute('stroke', color);
        path.setAttribute('stroke-width', '2');
        path.setAttribute('stroke-linejoin', 'round');
        path.setAttribute('stroke-linecap', 'round');
        if (clip) path.setAttribute('clip-path', 'url(#' + clip + ')');
        svg.appendChild(path);
      };
      if (hasZero && sr.key === 'kw') {
        // потребление — цветом серии, рекуперация — зелёным
        mk(sr.color || 'var(--accent)', 'clipAbove');
        mk('#34c759', 'clipBelow');
      } else {
        mk(sr.color || 'var(--accent)', null);
      }
    });

    const text = (x, y, str, anchor, cls) => {
      const e = document.createElementNS(SVGNS, 'text');
      e.setAttribute('x', x); e.setAttribute('y', y);
      e.setAttribute('font-size', '10');
      e.setAttribute('fill', 'var(--muted)');
      e.setAttribute('text-anchor', anchor || 'start');
      e.textContent = str;
      svg.appendChild(e);
    };
    text(padL - 4, padT + 4, fmt(max, 1), 'end');
    text(padL - 4, padT + plotH + 3, fmt(min, 1), 'end');
    if (hasZero) text(padL - 4, yZero + 3, '0', 'end');
    if (opts.label) text(W - padR, padT - 6, opts.label, 'end');

    if (hasTime) {
      const clock = (ts) => {
        const d = new Date(ts * 1000);
        const hh = String(d.getHours()).padStart(2, '0');
        const mm = String(d.getMinutes()).padStart(2, '0');
        return longSpan
          ? d.getDate().toString().padStart(2, '0') + '.' + String(d.getMonth() + 1).padStart(2, '0') + ' ' + hh + ':' + mm
          : hh + ':' + mm;
      };
      const marks = 5;
      for (let i = 0; i < marks; i++) {
        const ts = t0 + (t1 - t0) * (i / (marks - 1));
        const x = padL + plotW * (i / (marks - 1));
        text(x, H - 6, clock(ts), i === 0 ? 'start' : (i === marks - 1 ? 'end' : 'middle'));
      }

      // ---- подсказка: линия, точки на кривых и плашка со значениями
      const vline = document.createElementNS(SVGNS, 'path');
      vline.setAttribute('stroke', 'var(--muted)');
      vline.setAttribute('stroke-width', '1');
      vline.setAttribute('stroke-dasharray', '3 3');
      vline.setAttribute('fill', 'none');
      vline.setAttribute('visibility', 'hidden');
      svg.appendChild(vline);

      const dots = series.map(sr => {
        const c = document.createElementNS(SVGNS, 'circle');
        c.setAttribute('r', '3.5');
        c.setAttribute('fill', sr.color || 'var(--accent)');
        c.setAttribute('stroke', 'var(--surface)');
        c.setAttribute('stroke-width', '1.5');
        c.setAttribute('visibility', 'hidden');
        svg.appendChild(c);
        return c;
      });

      const bubbleBg = document.createElementNS(SVGNS, 'rect');
      bubbleBg.setAttribute('rx', '5');
      bubbleBg.setAttribute('fill', 'var(--surface-2)');
      bubbleBg.setAttribute('stroke', 'var(--line)');
      bubbleBg.setAttribute('height', '18');
      bubbleBg.setAttribute('visibility', 'hidden');
      svg.appendChild(bubbleBg);

      const bubble = document.createElementNS(SVGNS, 'text');
      bubble.setAttribute('font-size', '10.5');
      bubble.setAttribute('fill', 'var(--text)');
      bubble.setAttribute('visibility', 'hidden');
      svg.appendChild(bubble);

      const hit = document.createElementNS(SVGNS, 'rect');
      hit.setAttribute('x', padL);
      hit.setAttribute('y', padT);
      hit.setAttribute('width', plotW);
      hit.setAttribute('height', plotH);
      hit.setAttribute('fill', 'transparent');
      // none: палец не прокручивает страницу, пока ведём по графику
      hit.setAttribute('style', 'cursor:crosshair;touch-action:none');
      svg.appendChild(hit);

      const clockFull = (ts) => {
        const d = new Date(ts * 1000);
        return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
      };
      const unit = { kw: ' кВт', spd: ' км/ч', soc: ' %', batt: ' °C' };

      const hide = () => {
        vline.setAttribute('visibility', 'hidden');
        bubble.setAttribute('visibility', 'hidden');
        bubbleBg.setAttribute('visibility', 'hidden');
        dots.forEach(d => d.setAttribute('visibility', 'hidden'));
      };

      const show = (ev) => {
        const r = svg.getBoundingClientRect();
        const px = (ev.clientX - r.left) * (W / r.width);
        if (px < padL - 4 || px > W - padR + 4) return hide();
        const f = Math.min(1, Math.max(0, (px - padL) / plotW));
        const ts = t0 + f * (t1 - t0);
        const x = padL + f * plotW;

        vline.setAttribute('d', `M${x.toFixed(1)} ${padT} V${padT + plotH}`);
        vline.setAttribute('visibility', 'visible');

        const parts = [];
        series.forEach((sr, idx) => {
          const dot = dots[idx];
          if (!sr.show || !sr._pts.length) { dot.setAttribute('visibility', 'hidden'); return; }
          // значение в точке под пальцем: линейная интерполяция между соседями,
          // чтобы кружок ехал ровно за курсором, а не прыгал по точкам
          const pts = sr._pts;
          if (!pts.length) { dot.setAttribute('visibility', 'hidden'); return; }
          let v;
          if (ts <= pts[0][0]) v = pts[0][1];
          else if (ts >= pts[pts.length - 1][0]) v = pts[pts.length - 1][1];
          else {
            let i = 1;
            while (i < pts.length && pts[i][0] < ts) i++;
            const a = pts[i - 1], b = pts[i];
            const k = (ts - a[0]) / Math.max(1, b[0] - a[0]);
            v = a[1] + (b[1] - a[1]) * k;
          }
          dot.setAttribute('cx', x.toFixed(1));
          dot.setAttribute('cy', yFor(v).toFixed(1));
          dot.setAttribute('fill', (hasZero && sr.key === 'kw' && v < 0) ? '#34c759' : (sr.color || 'var(--accent)'));
          dot.setAttribute('visibility', 'visible');
          parts.push(fmt(v, 1) + unit[sr.key]);
        });

        const labelText = clockFull(ts) + (parts.length ? ' · ' + parts.join(' · ') : '');
        bubble.textContent = labelText;
        const tw = labelText.length * 5.6 + 10;
        const bx = Math.min(W - padR - tw, Math.max(padL, x - tw / 2));
        bubbleBg.setAttribute('x', bx);
        bubbleBg.setAttribute('y', padT - 14);
        bubbleBg.setAttribute('width', tw);
        bubble.setAttribute('x', bx + 5);
        bubble.setAttribute('y', padT - 1);
        bubbleBg.setAttribute('visibility', 'visible');
        bubble.setAttribute('visibility', 'visible');
      };

      hit.addEventListener('pointerdown', (ev) => { ev.preventDefault(); show(ev); });
      hit.addEventListener('pointermove', (ev) => { ev.preventDefault(); show(ev); });
      hit.addEventListener('pointerleave', hide);
      hit.addEventListener('pointerup', hide);
      hit.addEventListener('pointercancel', hide);
    }

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
    // Климат считаем отдельно от поездок: стоянка с включённой машиной.
    if (d.climate && d.climate.count > 0) {
      rows.push(['Климат', fmt(d.climate.kwh, 2) + ' кВт·ч']);
      rows.push(['Климат, время', fmt(d.climate.hours, 1) + ' ч']);
    }
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
    const LABELS = {
      kw: 'Мощность, кВт',
      spd: 'Скорость, км/ч',
      soc: 'Заряд, %',
      batt: 'Батарея, °C',
    };
    // Поездка: мощность, скорость, заряд и температура. Зарядка: без скорости,
    // там важнее температура батареи.
    const keys = kind === 'trip' ? ['kw', 'spd', 'soc', 'batt'] : ['kw', 'batt', 'soc'];
    const state = kind === 'trip'
      ? { kw: true, spd: false, soc: true, batt: false }
      : { kw: true, spd: false, soc: true, batt: true };

    box.innerHTML = `
      <div style="display:flex;gap:6px;margin-bottom:8px;flex-wrap:wrap">
        ${keys.map(k => `<button data-k="${k}" class="lc-btn" style="padding:6px 10px;border-radius:14px;border:1px solid var(--line);background:${state[k] ? 'var(--accent)' : 'var(--surface)'};color:${state[k] ? '#fff' : 'var(--text)'};font-size:12px">${LABELS[k]}</button>`).join('')}
      </div>
      <div class="lc-chart"></div>`;
    item.appendChild(box);
    item.dataset.expanded = '1';

    let pts = [];
    try {
      pts = (await api(`/api/samples?device=&from=${s}&to=${e}`)).points || [];
    } catch { }

    // Точки храним вместе со временем: по нему подписывается ось X.
    const series = { kw: [], spd: [], batt: [], soc: [] };
    pts.forEach(p => {
      const ts = Number(p.ts) || 0;
      // при зарядке ток отрицательный, поэтому мощность приходит со знаком минус
      if (p.kw !== undefined && p.kw !== null) series.kw.push([ts, kind === 'charge' ? Math.abs(Number(p.kw)) : Number(p.kw)]);
      if (p.spd !== undefined && p.spd !== null) series.spd.push([ts, Number(p.spd)]);
      // -99 значит «BMS не ответил» — такую точку в график не берём
      if (p.batt !== undefined && p.batt !== null && Number(p.batt) > -50) series.batt.push([ts, Number(p.batt)]);
      if (p.soc !== undefined && p.soc !== null) series.soc.push([ts, Number(p.soc)]);
    });

    const colors = { kw: 'var(--accent)', spd: '#34c759', batt: '#ff9500', soc: '#af52de' };

    function draw() {
      const sel = Object.keys(state).filter(k => state[k] && series[k].length);
      const el = $('.lc-chart', box);
      if (!sel.length) {
        el.innerHTML = '<div style="font-size:12px;color:var(--muted)">Нет данных за эту поездку.</div>';
        return;
      }
      chart(el, sel.map(k => ({ key: k, show: true, points: series[k], color: colors[k] })), { minSpan: 2, from: s, to: e });
      el.insertAdjacentHTML('beforeend',
        '<div style="display:flex;gap:12px;font-size:11px;color:var(--muted);margin-top:4px">' +
        sel.map(k => `<span><i style="display:inline-block;width:8px;height:8px;border-radius:2px;background:${colors[k]};margin-right:4px"></i>${k === 'kw' ? 'кВт' : k === 'spd' ? 'км/ч' : k === 'batt' ? '°C' : '%'}</span>`).join('') +
        '</div>');
    }

    if (window.ResizeObserver) {
      let lastW = 0;
      const ro = new ResizeObserver(() => {
        const w = $('.lc-chart', box)?.clientWidth || 0;
        if (w && Math.abs(w - lastW) > 8) { lastW = w; draw(); }
      });
      ro.observe(box);
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
        // кнопки и сам график живут внутри .lc-detail — их клики не должны
        // сворачивать карточку (из-за этого переключатели «схлопывали» её)
        if (e.target.closest && e.target.closest('.lc-detail')) return;
        const it = e.target.closest && e.target.closest('.trip-item');
        if (it) expand(it, 'trip');
      });
    }
    if (charges && !charges.dataset.lcBound) {
      charges.dataset.lcBound = '1';
      charges.addEventListener('click', (e) => {
        if (e.target.closest && e.target.closest('.lc-detail')) return;
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
      <style>
        details.lc-acc>summary{list-style:none;cursor:pointer;display:flex;align-items:center;gap:10px}
        details.lc-acc>summary::-webkit-details-marker{display:none}
        details.lc-acc>summary .lc-chev{transition:transform .15s}
        details.lc-acc[open]>summary .lc-chev{transform:rotate(90deg)}
        /* Браузер подсвечивает поля пароля/автозаполнения своим белым фоном
           даже в тёмной теме — принудительно держим цвет поверхности. */
        #screen-settings input[type=password]{background:var(--surface);color:var(--text)}
        #screen-settings input:-webkit-autofill,
        #screen-settings input:-webkit-autofill:hover,
        #screen-settings input:-webkit-autofill:focus{
          -webkit-text-fill-color:var(--text);
          -webkit-box-shadow:0 0 0 1000px var(--surface) inset;
          caret-color:var(--text);
        }
      </style>
      <div class="card" style="margin-top:8px">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Настройки платы</div></div>
        <div id="cfg-info" style="font-size:12px;color:var(--muted)"></div>
        <div style="font-size:11px;color:var(--muted);margin-top:6px">Разделы ниже раскрываются нажатием.</div>
      </div>
      <div id="cfg-fields"></div>
      <div class="card">
        <button id="cfg-save" style="width:100%;padding:12px;border:0;border-radius:8px;background:var(--accent);color:#fff;font-weight:600">Применить настройки на плате</button>
        <div id="cfg-hint" style="font-size:12px;color:var(--muted);margin-top:8px"></div>
      </div>
      <div class="card">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Название машины</div></div>
        <input id="cfg-name" placeholder="Мой Leaf" style="width:100%;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--surface);color:var(--text)">
        <button id="cfg-rename" style="margin-top:10px;width:100%;padding:12px;border:0;border-radius:8px;background:var(--surface-2);color:var(--text);font-weight:600">Сохранить название</button>
      </div>
      <div class="card">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Обратная связь</div></div>
        <div style="font-size:12px;color:var(--muted);margin-bottom:8px">Что не работает или чего не хватает — сообщение увидит администратор сервера.</div>
        <textarea id="fb-text" rows="4" maxlength="2000" placeholder="Опишите проблему или пожелание"
          style="width:100%;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--surface);color:var(--text);resize:vertical"></textarea>
        <button id="fb-send" style="margin-top:10px;width:100%;padding:12px;border:0;border-radius:8px;background:var(--accent);color:#fff;font-weight:600">Отправить</button>
        <div id="fb-hint" style="font-size:12px;color:var(--muted);margin-top:8px"></div>
      </div>`;
    screens.insertBefore(sec, $('#screen-more') || null);

    setTimeout(() => {
      const btn = document.getElementById('fb-send');
      if (!btn || btn.dataset.bound) return;
      btn.dataset.bound = '1';
      btn.onclick = async () => {
        const ta = document.getElementById('fb-text');
        const hint = document.getElementById('fb-hint');
        const text = (ta.value || '').trim();
        if (text.length < 5) { hint.textContent = 'Напишите хотя бы пару слов'; return; }
        btn.disabled = true;
        try {
          await api('/api/feedback', { text });
          ta.value = '';
          hint.textContent = 'Спасибо, отправлено';
        } catch (e) {
          hint.textContent = e.message;
        } finally { btn.disabled = false; }
      };
    }, 0);
  }

  async function showNotices() {
    try {
      const res = await api('/api/notices');
      const items = (res.items || []).filter(x => !x.read_at);
      const more = $('#screen-more');
      if (!items.length || !more) return;
      let box = document.getElementById('lc-notices');
      if (!box) {
        box = document.createElement('div');
        box.id = 'lc-notices';
        box.className = 'mg';
        more.insertBefore(box, more.querySelector('.mg') || null);
      }
      box.innerHTML =
        '<div class="mg-title">Сообщения</div><div class="mg-card">' +
        items.map(x => `
          <div class="more-item" style="cursor:default;align-items:flex-start">
            <div class="mi">${MORE_ICONS.bell}</div>
            <div class="mg-txt">
              <div class="mg-t" style="white-space:normal;font-size:14px">${(x.text || '').replace(/</g, '&lt;')}</div>
              <div class="mg-s" style="margin-top:4px">${admWhen(x.created_at)}</div>
              <button data-notice="${x.id}" style="margin-top:8px;border:0;border-radius:8px;background:var(--surface-2);color:var(--text);padding:7px 11px;font-size:12px">Понятно</button>
            </div>
          </div>`).join('') + '</div>';
      $$('#lc-notices [data-notice]').forEach(b => b.onclick = async () => {
        try {
          await api('/api/notices/read', { id: Number(b.dataset.notice) });
          const row = b.closest('.more-item');
          if (row) row.remove();
        } catch (e) { /* тихо */ }
      });
      toast(items.length === 1 ? 'Есть новое сообщение' : 'Есть новые сообщения');
    } catch (e) { /* тихо */ }
  }

  /* ---------- лимит заряда на главной ---------- */
  async function initGarageLimit() {
    const btn = document.getElementById('g-limit-btn');
    const val = document.getElementById('g-limit-val');
    if (!btn || !val || btn.dataset.lcBound) return;
    btn.dataset.lcBound = '1';
    let current = null;
    try {
      const c = await api('/api/config');
      current = Number((c.config || {}).charge_limit);
    } catch (e) { /* плата ещё не присылала настройки */ }
    const show = () => {
      if (!Number.isFinite(current) || current === null) val.textContent = '—';
      else val.textContent = current ? current + ' %' : 'выключен';
    };
    show();
    btn.onclick = async () => {
      const def = Number.isFinite(current) && current ? String(current) : '85';
      const input = prompt('Остановить зарядку при заряде, %\n(0 — выключить лимит):', def);
      if (input === null) return;
      const n = parseInt(input, 10);
      if (!Number.isFinite(n) || n < 0 || n > 100) { toast('Нужно число от 0 до 100'); return; }
      try {
        await api('/api/config', { key: 'charge_limit', value: n });
        current = n;
        show();
        toast(n ? 'Лимит ' + n + '% — плата применит при связи' : 'Лимит зарядки выключен');
      } catch (e) { alert(e.message); }
    };
  }

  /* ---------- климат по расписанию ---------- */
  async function refreshClimPlan() {
    const hint = document.getElementById('clim-plan-hint');
    if (!hint) return;
    try {
      const r = await api('/api/sched');
      const pend = (r.schedules || []).filter(x => x.status === 'waiting');
      if (!pend.length) { hint.textContent = ''; return; }
      const x = pend[0];
      const t = new Date(x.at_ts * 1000);
      const hh = String(t.getHours()).padStart(2, '0');
      const mm = String(t.getMinutes()).padStart(2, '0');
      hint.innerHTML = 'Запланировано на ' + hh + ':' + mm +
        ' <button id="clim-cancel" style="border:0;background:none;color:var(--danger);font-size:12px;text-decoration:underline;cursor:pointer">отменить</button>';
      const c = document.getElementById('clim-cancel');
      if (c) c.onclick = async () => {
        try {
          await api('/api/sched/cancel', { id: x.id });
          toast('Расписание отменено');
          refreshClimPlan();
        } catch (e) { alert(e.message); }
      };
    } catch (e) { hint.textContent = ''; }
  }

  function initClimPlan() {
    const btn = document.getElementById('clim-plan');
    if (!btn || btn.dataset.lcBound) return;
    btn.dataset.lcBound = '1';
    btn.onclick = async () => {
      const time = (document.getElementById('clim-at') || {}).value || '08:00';
      const parts = time.split(':');
      const now = new Date();
      const at = new Date(now.getFullYear(), now.getMonth(), now.getDate(),
                          parseInt(parts[0], 10) || 0, parseInt(parts[1], 10) || 0);
      if (at <= now) at.setDate(at.getDate() + 1);
      const minutes = Number((document.getElementById('hvac-minutes') || {}).value || 15);
      try {
        await api('/api/sched', { at: Math.floor(at.getTime() / 1000), minutes });
        toast('Климат включится в ' + time + ' на ' + minutes + ' мин');
        refreshClimPlan();
      } catch (e) { alert(e.message); }
    };
    refreshClimPlan();
  }

  /* ---------- единый стиль подэкранов (как во «Ещё») ---------- */
  function toGroups(id) {
    const sec = document.getElementById(id);
    if (!sec || sec.dataset.lcGroups) return;
    sec.dataset.lcGroups = '1';
    sec.querySelectorAll(':scope > .card').forEach(card => {
      const head = card.querySelector(':scope > .card-header');
      if (!head) return;
      const title = (head.textContent || '').trim();
      const group = document.createElement('div');
      group.className = 'lg';
      const t = document.createElement('div');
      t.className = 'lg-title';
      t.textContent = title;
      const body = document.createElement('div');
      body.className = 'lg-card';
      while (card.firstChild) body.appendChild(card.firstChild);
      const old = body.querySelector(':scope > .card-header');
      if (old) old.remove();
      group.appendChild(t);
      group.appendChild(body);
      card.replaceWith(group);
    });
  }

  /* ---------- переключение дизайна ---------- */
  function initDesignSwitch() {
    const row = document.getElementById('design-switch');
    if (row && !row.dataset.lcBound) {
      row.dataset.lcBound = '1';
      row.addEventListener('click', () => {
        if (!confirm('Переключиться на классический дизайн?\n\nВыбор запомнится, вернуть новый можно будет там же в «Ещё».')) return;
        try { localStorage.setItem('leafDesign', 'classic'); } catch (e) { /* ignore */ }
        location.href = '/classic.html';
      });
    }
    try {
      if (localStorage.getItem('leafDesign') === 'classic' &&
          (location.pathname === '/' || location.pathname === '/index.html' ||
           location.pathname === '/app')) {
        location.replace('/classic.html');
      }
    } catch (e) { /* ignore */ }
  }

  function styleSubScreens() {
    ['screen-admin', 'screen-settings', 'screen-claim', 'screen-fw', 'screen-stats'].forEach(toGroups);
  }

  function showSettings() {
    styleSubScreens();
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
    ['sleep_after', 'Сон при бездействии через, ч', 'пока машина активна, плата не спит'],
    ['wake_fast', 'После этого — пробуждение каждые, мин', 'пока простой меньше 7 дней'],
    ['wake_slow', 'После 7 дней простоя — пробуждение каждые, мин', ''],
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

    const card = (title, desc, body, open) => `
      <details class="card lc-acc"${open ? ' open' : ''}>
        <summary>
          <div style="flex:1">
            <div style="font-size:16px">${title}</div>
            ${desc ? `<div style="font-size:12px;color:var(--muted);margin-top:2px">${desc}</div>` : ''}
          </div>
          <div class="lc-chev" style="color:var(--muted);font-size:20px;line-height:1">›</div>
        </summary>
        <div style="margin-top:12px">${body}</div>
      </details>`;
    const numField = (k, t, h) => `
      <label style="display:block;font-size:12px;color:var(--muted);margin-bottom:10px">${t}
        <input data-num="${k}" type="number" value="${cfg[k] !== undefined ? cfg[k] : ''}" style="${inputStyle}">
        ${h ? `<span style="font-size:11px">${h}</span>` : ''}
      </label>`;
    const boolRow = (k, t, h) => `
      <label style="display:flex;align-items:center;gap:10px;padding:8px 0">
        <input data-bool="${k}" type="checkbox" ${cfg[k] ? 'checked' : ''} style="width:18px;height:18px">
        <span style="font-size:14px">${t}${h ? `<span style="display:block;font-size:11px;color:var(--muted)">${h}</span>` : ''}</span>
      </label>`;
    const textField = (k, t, ph, value, type) => `
      <label style="display:block;font-size:12px;color:var(--muted);margin-bottom:10px">${t}
        <input data-text="${k}" type="${type || 'text'}" value="${String(value === undefined || value === null ? '' : value).replace(/"/g, '&quot;')}" placeholder="${ph || ''}" style="${inputStyle}">
      </label>`;

    fields.innerHTML =
      card('Режим работы', 'Как плата связана с интернетом и сервером.',
        `<label style="display:flex;align-items:flex-start;gap:10px;padding:8px 0">
           <input type="radio" name="cfgmode" value="1" ${cfg.remote_mode === 1 ? 'checked' : ''} style="width:18px;height:18px;margin-top:2px">
           <span style="font-size:14px">Сервер — всегда на связи
             <span style="display:block;font-size:11px;color:var(--muted)">данные и команды идут через сервер, звонки работают</span></span>
         </label>
         <label style="display:flex;align-items:flex-start;gap:10px;padding:8px 0">
           <input type="radio" name="cfgmode" value="0" ${cfg.remote_mode === 0 ? 'checked' : ''} style="width:18px;height:18px;margin-top:2px">
           <span style="font-size:14px">Без подключения — только плата
             <span style="display:block;font-size:11px;color:var(--muted)">локальная страница и приём звонков, в интернет не выходит</span></span>
         </label>`, true) +
      card('Зарядка', 'Остановка заряда по проценту с приборки.',
        numField('charge_limit', 'Лимит заряда, %', '0 — выключено, обычно 70–95')) +
      card('Сон и пробуждение', 'Плата засыпает, когда машина долго стоит, и периодически просыпается на связь.',
        boolRow('sleep', 'Глубокий сон', '') +
        numField('sleep_after', 'Сон при бездействии через, ч', 'пока машина активна, плата не спит') +
        numField('wake_fast', 'После этого — пробуждение каждые, мин', 'пока простой меньше 7 дней') +
        numField('wake_slow', 'После 7 дней простоя — пробуждение каждые, мин', '')) +
      card('Интернет через SIM', 'Резервный канал, когда Wi-Fi нет. Нужен APN оператора.',
        boolRow('modem', 'Модем SIM800', 'интернет и приём звонков') +
        textField('apn', 'APN (точка доступа)', 'internet.beeline.ru', cfg.apn) +
        textField('apn_user', 'Логин APN', 'обычно не нужен', cfg.apn_user) +
        textField('apn_pass', 'Пароль APN', 'не менять', '', 'password') +
        '<div style="font-size:11px;color:var(--muted)">пароль не отображается; пустое поле — не менять</div>') +
      card('Плата в сети', 'Имя, под которым плата видна в домашней сети.',
        textField('host_name', 'Имя в сети (DHCP/mDNS)', 'leaf', cfg.host_name || 'leaf') +
        '<div style="font-size:11px;color:var(--muted)">применится после перезагрузки платы</div>') +
      card('Дополнительно', 'Редко нужные функции.',
        boolRow('locking', 'Блокировка кнопок', 'запрет команд, если выключено') +
        boolRow('uds', 'Расширенная диагностика', 'UDS-запросы VCM/BCM/ABS/TMI') +
        boolRow('uds_tpms', 'TPMS', 'если установлен модуль давления'));

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
      const modeEl = document.querySelector('#cfg-fields input[name=cfgmode]:checked');
      if (modeEl) jobs.unshift(['remote_mode', Number(modeEl.value)]);
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
      if (t.includes('Обновление прошивки')) {
        el.dataset.lcHooked = '1';
        el.addEventListener('click', (e) => {
          e.preventDefault();
          e.stopImmediatePropagation();
          showFirmware();
        }, true);
        return;
      }
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

  /* ---------- 4. выход и админка ---------- */
  let userRole = null;

  async function loadRole() {
    try {
      const d = await api('/api/devices');
      userRole = d.role || 'owner';
    } catch { userRole = null; }
  }

  const MORE_ICONS = {
    plus: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"><path d="M12 5v14M5 12h14"/></svg>',
    bolt: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M13 2L3 14h9l-1 8 10-12h-9l1-8z"/></svg>',
    gear: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 00.3 1.8l.1.1a2 2 0 11-2.8 2.8l-.1-.1a1.7 1.7 0 00-1.8-.3 1.7 1.7 0 00-1 1.5V21a2 2 0 11-4 0v-.1a1.7 1.7 0 00-1-1.5 1.7 1.7 0 00-1.8.3l-.1.1a2 2 0 11-2.8-2.8l.1-.1a1.7 1.7 0 00.3-1.8 1.7 1.7 0 00-1.5-1H3a2 2 0 110-4h.1a1.7 1.7 0 001.5-1 1.7 1.7 0 00-.3-1.8l-.1-.1a2 2 0 112.8-2.8l.1.1a1.7 1.7 0 001.8.3H9a1.7 1.7 0 001-1.5V3a2 2 0 114 0v.1a1.7 1.7 0 001 1.5 1.7 1.7 0 001.8-.3l.1-.1a2 2 0 112.8 2.8l-.1.1a1.7 1.7 0 00-.3 1.8V9a1.7 1.7 0 001.5 1H21a2 2 0 110 4h-.1a1.7 1.7 0 00-1.5 1z"/></svg>',
    chart: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M4 19h16M7 15V9m5 6V5m5 10v-4"/></svg>',
    down: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M12 3v12m0 0l-4-4m4 4l4-4M5 21h14"/></svg>',
    install: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><rect x="6" y="2" width="12" height="20" rx="2"/><path d="M12 8v6m0 0l-2.5-2.5M12 14l2.5-2.5"/></svg>',
    shield: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z"/></svg>',
    out: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"><path d="M9 21H5a2 2 0 01-2-2V5a2 2 0 012-2h4M16 17l5-5-5-5M21 12H9"/></svg>',
    bell: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M18 8a6 6 0 10-12 0c0 7-3 7-3 9h18c0-2-3-2-3-9"/><path d="M10 21h4"/></svg>'
  };

  function moreRow(title, sub, icon, onClick, opts) {
    opts = opts || {};
    const row = document.createElement('div');
    row.className = 'more-item' + (opts.danger ? ' mg-danger' : '');
    row.innerHTML =
      `<div class="mi${opts.gray ? ' gray' : ''}${opts.danger ? ' red' : ''}">${icon || ''}</div>` +
      `<div class="mg-txt"><div class="mg-t">${title}</div>` +
      `${sub ? `<div class="mg-s">${sub}</div>` : ''}</div>` +
      `${opts.value ? `<div class="mg-val">${opts.value}</div>` : ''}` +
      `${onClick ? '<div class="chev"><svg viewBox="0 0 24 24" fill="currentColor"><path d="M10 6L8.59 7.41 13.17 12l-4.58 4.59L10 18l6-6z"/></svg></div>' : ''}`;
    if (onClick) {
      row.addEventListener('click', (e) => {
        e.preventDefault();
        e.stopImmediatePropagation();
        onClick();
      }, true);
    }
    return row;
  }

  async function renderMoreProfile() {
    const box = $('#more-profile');
    if (!box) return;
    let name = '—';
    let role = 'Пользователь';
    let online = false;
    try {
      const me = await api('/api/auth/me');
      if (me && me.user) {
        name = me.user.username || name;
        role = me.user.role === 'admin' ? 'Администратор' : 'Владелец';
      }
    } catch (e) { /* не критично */ }
    try {
      const st = await api('/api/status');
      online = !!(st && st.online);
    } catch (e) { /* не критично */ }
    box.innerHTML =
      `<div class="ava">${(name[0] || '?').toUpperCase()}</div>` +
      `<div><div class="nm">${name}</div><div class="mt">${role} · через сервер</div></div>` +
      `<span class="pill">${online ? 'плата на связи' : 'плата офлайн'}</span>`;
  }

  const toggleCarProfile = async () => {
    try {
      const c = await api('/api/config');
      const cur = Number((c.config || {}).vehicle_profile || 0);
      const names = ['авто', 'ZE1 (2018+)', 'AZE0/ZE0 (2011–2017)'];
      const next = (cur + 1) % 3;
      const ok = confirm('Сейчас: ' + names[cur] + '\nПереключить на: ' + names[next] +
        '?\n\nПлата применит при следующей связи (до 30 секунд).');
      if (!ok) return;
      await api('/api/config', { key: 'vehicle_profile', value: next });
      toast('Профиль: ' + names[next]);
    } catch (e) { alert(e.message); }
  };

  function addMoreRows() {
    const car = $('#more-car');
    const data = $('#more-data');
    const service = $('#more-service');
    const outCard = $('#more-logout-card');
    if (!car || !data) return;

    if (!car.querySelector('[data-lc-claim]')) {
      const r = moreRow('Добавить машину', 'привязать плату по коду', MORE_ICONS.plus, showClaim);
      r.setAttribute('data-lc-claim', '1');
      car.appendChild(r);
    }
    if (!car.querySelector('[data-lc-profile]')) {
      const r = moreRow('Профиль машины', 'ZE1 / AZE0 — переключить', MORE_ICONS.bolt, toggleCarProfile);
      r.setAttribute('data-lc-profile', '1');
      car.appendChild(r);
    }
    if (!car.querySelector('[data-lc-settings]')) {
      const r = moreRow('Настройки подключения', 'Wi-Fi, SIM, тарифы, лимит заряда', MORE_ICONS.gear, showSettings);
      r.setAttribute('data-lc-settings', '1');
      const t = r.querySelector('.mg-t');
      if (t) t.id = 'more-settings-label';
      car.appendChild(r);
    }

    if (userRole === 'admin' && service && !service.querySelector('[data-lc-admin]')) {
      const r = moreRow('Администрирование', 'устройства, пользователи, журнал', MORE_ICONS.shield, showAdmin);
      r.setAttribute('data-lc-admin', '1');
      service.insertBefore(r, service.firstChild);
    }

    if (!data.querySelector('[data-lc-xlsx]')) {
      const r = moreRow('История в Excel', 'обзор, дни, сессии и графики', MORE_ICONS.chart, () => {
        location.href = '/api/export/xlsx?days=90';
      });
      r.setAttribute('data-lc-xlsx', '1');
      data.appendChild(r);
    }
    if (!data.querySelector('[data-lc-apk]')) {
      const r = moreRow('Скачать приложение', 'Android, файл APK', MORE_ICONS.down, () => {
        location.href = '/app.apk';
      }, { gray: true });
      r.setAttribute('data-lc-apk', '1');
      data.appendChild(r);
    }
    if (!data.querySelector('[data-lc-install]') && !isStandalone()) {
      const r = moreRow('Установить как приложение', 'ярлык на экран телефона', MORE_ICONS.install, installPwa, { gray: true });
      r.setAttribute('data-lc-install', '1');
      data.appendChild(r);
    }

    if (outCard && !outCard.querySelector('[data-lc-logout]')) {
      const r = moreRow('Выйти', 'сменить пользователя', MORE_ICONS.out, async () => {
        try { await api('/api/auth/logout', {}); } catch (e) { /* ignore */ }
        location.href = '/login';
      }, { danger: true });
      r.setAttribute('data-lc-logout', '1');
      outCard.appendChild(r);
    }

    renderMoreProfile();
  }

  function addAdminScreen() {
    const screens = $('#screens');
    if (!screens || $('#screen-admin')) return;
    const sec = document.createElement('section');
    sec.className = 'screen';
    sec.id = 'screen-admin';
    sec.innerHTML = `
      <div class="card" style="margin-top:8px">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Заявки на подключение</div></div>
        <div id="adm-pending"></div>
      </div>
      <div class="card">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Обращения владельцев</div></div>
        <div id="adm-feedback"></div>
      </div>
      <div class="card">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Пользователи</div></div>
        <div id="adm-users"></div>
      </div>
      <div class="card">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Устройства</div></div>
        <div id="adm-devices"></div>
      </div>
      <div class="card">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">CAN-сессии для разбора</div></div>
        <div style="font-size:12px;color:var(--muted);margin-bottom:6px">
          Плата пишет шину во время поездки, климата и зарядки, ещё 5 минут после и сама выгружает сюда.
          TXT/CSV — расшифровка, BIN — сырые записи.
        </div>
        <div id="adm-canlog"></div>
      </div>
      <div class="card">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Журнал</div></div>
        <div id="adm-events"></div>
      </div>`;
    screens.insertBefore(sec, $('#screen-more') || null);
  }

  function showAdmin() {
    styleSubScreens();
    $$('.screen').forEach(s => s.classList.remove('active'));
    const sec = $('#screen-admin');
    if (sec) sec.classList.add('active');
    $$('.tab').forEach(t => t.classList.remove('active'));
    const sc = $('#screens'); if (sc) sc.scrollTop = 0;
    loadAdmin();
  }

  const admBleed = (id) => {
    const el = document.getElementById(id);
    if (el && el.parentElement && el.parentElement.classList.contains('lg-card')) {
      el.parentElement.classList.add('tbl-card');
    }
  };

  const admBtn = (label, color) =>
    `border:0;border-radius:8px;background:var(--surface-2);color:${color || 'var(--text)'};padding:7px 11px;font-size:12px`;
  const admWhen = (sec) => {
    if (!sec) return '—';
    const d = new Date(sec * 1000);
    return d.toLocaleString('ru-RU', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' });
  };
  const admAgo = (sec) => {
    if (!sec) return 'никогда';
    const s = Math.max(0, Math.round(Date.now() / 1000 - sec));
    if (s < 90) return 'только что';
    if (s < 3600) return Math.round(s / 60) + ' мин назад';
    if (s < 86400) return Math.round(s / 3600) + ' ч назад';
    return Math.round(s / 86400) + ' дн назад';
  };

  async function loadAdmin() {
    try {
      // ---- заявки
      const p = await api('/api/admin/devices?status=pending');
      const pend = p.devices || [];
      $('#adm-pending').innerHTML = pend.length
        ? `<div class="tbl-wrap"><table class="tbl">
            <thead><tr><th>Логин платы</th><th>Прошивка</th><th class="right">Действие</th></tr></thead>
            <tbody>${pend.map(d => `
              <tr>
                <td class="mono">${d.login}
                  <div class="adm-sub">${d.serial || ''}${d.claim_code ? ' · код ' + d.claim_code : ''}</div></td>
                <td class="mono">${(d.fw || '—').replace(/^FR-/, '')}</td>
                <td class="right"><div class="row-acts">
                  <button data-ok="${d.id}" class="adm-tbtn adm-tbtn-acc">Привязать</button>
                  <button data-no="${d.id}" class="adm-tbtn adm-tbtn-del">Удалить</button>
                </div></td>
              </tr>`).join('')}</tbody>
          </table></div>`
        : '<div class="adm-empty">Новых заявок нет — список обновится сам</div>';
      admBleed('adm-pending');

      $$('#adm-pending [data-ok]').forEach(b => b.onclick = async () => {
        const username = prompt('К какому пользователю привязать? (логин)');
        if (!username) return;
        try {
          await api('/api/admin/devices/' + b.dataset.ok + '/assign', { username });
          toast('Машина привязана к ' + username);
          loadAdmin();
        } catch (e) { alert(e.message); }
      });
      $$('#adm-pending [data-no]').forEach(b => b.onclick = async () => {
        if (!confirm('Удалить устройство вместе с историей?')) return;
        try {
          await api('/api/admin/devices/' + b.dataset.no, undefined, 'DELETE');
          toast('Устройство удалено');
          loadAdmin();
        } catch (e) { alert(e.message); }
      });

      // ---- обращения
      const fb = await api('/api/admin/feedback');
      const items = fb.items || [];
      const open = items.filter(x => x.status !== 'done').length;
      $('#adm-feedback').innerHTML = items.length
        ? items.map(x => `
          <div style="display:flex;gap:10px;padding:10px 0;border-bottom:1px solid var(--line);align-items:flex-start">
            <div style="flex:1">
              <div style="font-size:13px;white-space:pre-wrap">${(x.text || '').replace(/</g, '&lt;')}</div>
              <div style="font-size:12px;color:var(--muted);margin-top:4px">
                ${x.username || 'без пользователя'} · ${admWhen(x.created_at)}${x.status === 'done' ? '<span class="lg-badge ok">прочитано</span>' : '<span class="lg-badge warn">новое</span>'}</div>
            </div>
            <div class="row-acts" style="flex-shrink:0">
              ${x.status === 'done' ? '' : `<button data-fb-close="${x.id}" class="adm-tbtn adm-tbtn-acc">Закрыть и уведомить</button>`}
              <button data-fb-del="${x.id}" class="adm-tbtn adm-tbtn-del">Удалить</button>
            </div>
          </div>`).join('')
        : '<div style="font-size:13px;color:var(--muted)">Обращений нет</div>';
      $$('#adm-feedback [data-fb-close]').forEach(b => b.onclick = async () => {
        try {
          await api('/api/admin/feedback/' + b.dataset.fbClose, { action: 'close', notify: true });
          toast('Закрыто, пользователь получит сообщение');
          loadAdmin();
        } catch (e) { alert(e.message); }
      });
      $$('#adm-feedback [data-fb-del]').forEach(b => b.onclick = async () => {
        if (!confirm('Удалить обращение без уведомления?')) return;
        try {
          await api('/api/admin/feedback/' + b.dataset.fbDel, undefined, 'DELETE');
          toast('Обращение удалено');
          loadAdmin();
        } catch (e) { alert(e.message); }
      });
      const badge = document.getElementById('adm-fb-badge');
      if (badge) { badge.textContent = open ? String(open) : ''; badge.style.display = open ? 'inline-block' : 'none'; }

      // ---- пользователи
      const u = await api('/api/admin/users');
      // технические аккаунты плат (логин = логин устройства) в списке не нужны
      const users = (u.users || []).filter(x => !x.is_device || (x.devices || 0) > 0);
      $('#adm-users').innerHTML = users.map(x => {
        const devs = x.device_list || [];
        const chips = devs.length
          ? devs.map(d => `<span style="display:inline-flex;align-items:center;gap:6px;margin:6px 8px 0 0;padding:5px 9px;border-radius:999px;background:var(--surface-2);font-size:12px">
              <i style="width:7px;height:7px;border-radius:50%;background:${d.online ? 'var(--accent)' : 'var(--muted)'}"></i>
              ${d.login}${d.fw ? ' · ' + d.fw.replace(/^FR-/, '') : ''}</span>`).join('')
          : '<span style="font-size:12px;color:var(--muted)">машин нет</span>';
        return `
        <div style="padding:12px 0;border-bottom:1px solid var(--line)">
          <div style="display:flex;gap:10px;align-items:center">
            <div style="flex:1">
              <div style="font-size:14px">${x.username}${x.feedback_open ? ` <span style="font-size:11px;color:#d9534f">● ${x.feedback_open}</span>` : ''}</div>
              <div style="font-size:12px;color:var(--muted)">${x.role} · создан ${admWhen(x.created_at)} · последняя связь ${admAgo(x.last_seen)}</div>
            </div>
            <button data-pw="${x.id}" style="${admBtn('Пароль')}">Пароль</button>
            ${x.role === 'admin' ? '' : `<button data-del-user="${x.id}" style="${admBtn('Удалить', 'var(--danger)')}">Удалить</button>`}
          </div>
          <div style="margin-top:2px">${chips}</div>
          ${devs.length ? `<div style="margin-top:8px">${devs.map(d =>
            `<button data-unbind="${d.id}" style="${admBtn('Отвязать ' + d.login, 'var(--danger)')};margin:0 6px 6px 0">Отвязать ${d.login}</button>`).join('')}</div>` : ''}
        </div>`;
      }).join('') + `
        <div style="margin-top:14px;padding-top:12px;border-top:1px solid var(--line)">
          <div style="font-size:12px;color:var(--muted);margin-bottom:6px">Создать клиента</div>
          <input id="adm-new-user" placeholder="логин" style="width:100%;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--surface);color:var(--text);margin-bottom:6px">
          <input id="adm-new-pass" placeholder="пароль (мин. 8)" style="width:100%;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--surface);color:var(--text);margin-bottom:6px">
          <button id="adm-create" style="width:100%;padding:11px;border:0;border-radius:8px;background:var(--accent);color:#fff;font-weight:600">Создать и выдать</button>
        </div>`;

      $$('#adm-users [data-pw]').forEach(b => b.onclick = async () => {
        const pwd = prompt('Новый пароль (минимум 8 символов):');
        if (!pwd) return;
        try {
          await api('/api/admin/users/' + b.dataset.pw + '/password', { password: pwd });
          toast('Пароль изменён');
        } catch (e) { alert(e.message); }
      });
      $$('#adm-users [data-del-user]').forEach(b => b.onclick = async () => {
        if (!confirm('Удалить аккаунт? Его машины отвяжутся и получат новые коды.')) return;
        try {
          const r = await api('/api/admin/users/' + b.dataset.delUser, undefined, 'DELETE');
          toast('Аккаунт удалён, отвязано машин: ' + (r.devices_unbound || 0));
          loadAdmin();
        } catch (e) { alert(e.message); }
      });
      $$('#adm-users [data-unbind]').forEach(b => b.onclick = async () => {
        if (!confirm('Отвязать плату от аккаунта? У неё появится новый код привязки.')) return;
        try {
          await api('/api/admin/devices/' + b.dataset.unbind + '/unbind', {});
          toast('Плата отвязана');
          loadAdmin();
        } catch (e) { alert(e.message); }
      });
      const createBtn = document.getElementById('adm-create');
      if (createBtn) createBtn.onclick = async () => {
        const username = (document.getElementById('adm-new-user').value || '').trim();
        const password = document.getElementById('adm-new-pass').value || '';
        if (!username || password.length < 8) { alert('Логин и пароль (мин. 8)'); return; }
        try {
          await api('/api/admin/users', { username, password });
          alert('Клиент создан.\n\nЛогин: ' + username + '\nПароль: ' + password +
                '\n\nПередайте эти данные клиенту. Машину он добавит кодом с платы.');
          loadAdmin();
        } catch (e) { alert(e.message); }
      };

      // ---- устройства
      const all = await api('/api/admin/devices?status=all');
      $('#adm-devices').innerHTML = (all.devices || []).length
        ? `<div class="tbl-wrap"><table class="tbl">
            <thead><tr><th>Устройство</th><th>Статус</th><th>Связь</th><th class="right">Действия</th></tr></thead>
            <tbody>${(all.devices || []).map(d => `
              <tr>
                <td class="mono">${d.login}
                  <div class="adm-sub">${d.serial || ''} · ${d.owner || 'без владельца'}${d.claim_code ? ' · код ' + d.claim_code : ''}</div></td>
                <td><span class="lg-badge ${d.online ? 'ok' : ''}">${d.online ? 'на связи' : 'офлайн'}</span></td>
                <td>${admAgo(d.last_seen)}</td>
                <td class="right"><div class="row-acts">
                  <button data-assign="${d.id}" class="adm-tbtn">Привязать</button>
                  ${d.owner ? `<button data-unbind="${d.id}" class="adm-tbtn adm-tbtn-del">Отвязать</button>` : ''}
                  <button data-del-dev="${d.id}" class="adm-tbtn adm-tbtn-del">Удалить</button>
                </div></td>
              </tr>`).join('')}</tbody>
          </table></div>`
        : '<div class="adm-empty">Устройств нет</div>';
      admBleed('adm-devices');

      $$('#adm-devices [data-assign]').forEach(b => b.onclick = async () => {
        const username = prompt('Логин пользователя:');
        if (!username) return;
        try {
          await api('/api/admin/devices/' + b.dataset.assign + '/assign', { username });
          toast('Привязано');
          loadAdmin();
        } catch (e) { alert(e.message); }
      });
      $$('#adm-devices [data-unbind]').forEach(b => b.onclick = async () => {
        if (!confirm('Отвязать плату от аккаунта?')) return;
        try {
          await api('/api/admin/devices/' + b.dataset.unbind + '/unbind', {});
          toast('Отвязано');
          loadAdmin();
        } catch (e) { alert(e.message); }
      });
      $$('#adm-devices [data-del-dev]').forEach(b => b.onclick = async () => {
        if (!confirm('Удалить устройство и всю его историю?')) return;
        try {
          await api('/api/admin/devices/' + b.dataset.delDev, undefined, 'DELETE');
          toast('Удалено');
          loadAdmin();
        } catch (e) { alert(e.message); }
      });

      // ---- CAN-сессии (тестовый режим)
      try {
        const cl = await api('/api/admin/canlog');
        const items = cl.items || [];
        const mb = (n) => (n / 1048576).toFixed(2).replace('.', ',') + ' МБ';
        $('#adm-canlog').innerHTML = items.length
          ? items.map(x => {
              const when = admWhen(x.started_at || x.uploaded_at);
              const link = (fmt, label) =>
                `<a href="/api/admin/canlog/${x.id}/download?fmt=${fmt}" style="${admBtn(label)};text-decoration:none;display:inline-block">${label}</a>`;
              return `<div style="display:flex;gap:8px;align-items:center;padding:10px 0;border-bottom:1px solid var(--line)">
                <div style="flex:1">
                  <div style="font-size:13px">${x.device || '—'} · ${when}</div>
                  <div style="font-size:12px;color:var(--muted)">${mb(x.size)} · ${x.frames} кадров${x.complete ? '' : ' · файл не дописан'}</div>
                </div>
                ${link('txt', 'TXT')}
                ${link('csv', 'CSV')}
                ${link('bin', 'BIN')}
                <button data-candel="${x.id}" style="${admBtn('×', 'var(--danger)')}">×</button>
              </div>`;
            }).join('')
          : '<div style="font-size:13px;color:var(--muted)">Пока пусто: запись появится после поездки или климата.</div>';
        $$('#adm-canlog [data-candel]').forEach(b => b.onclick = async () => {
          if (!confirm('Удалить CAN-сессию с сервера?')) return;
          try {
            await api('/api/admin/canlog/' + b.dataset.candel, undefined, 'DELETE');
            loadAdmin();
          } catch (e) { alert(e.message); }
        });
      } catch (e) { $('#adm-canlog').textContent = e.message; }

      // ---- журнал
      const ev = await api('/api/admin/events');
      const events = (ev.events || []).slice(0, 60);
      const evLabel = (k) => {
        if (k.startsWith('cmd_')) return 'команда';
        if (k.startsWith('admin_')) return 'админ';
        if (k.startsWith('device_')) return 'плата';
        if (k.startsWith('feedback')) return 'обращение';
        if (k.startsWith('config')) return 'настройки';
        if (k.startsWith('can')) return 'CAN';
        return 'событие';
      };
      const evTone = (k) => /delete|reject/i.test(k) ? 'danger'
        : /created|approve|assign|done|queued/i.test(k) ? 'ok'
        : /fail|error|offline/i.test(k) ? 'warn' : '';
      $('#adm-events').innerHTML = events.length
        ? events.map(e => {
            const d = new Date(e.ts * 1000);
            const hh = String(d.getHours()).padStart(2, '0');
            const mm = String(d.getMinutes()).padStart(2, '0');
            const same = d.toDateString() === new Date().toDateString();
            const when = same ? hh + ':' + mm
              : String(d.getDate()).padStart(2, '0') + '.' + String(d.getMonth() + 1).padStart(2, '0') + ' ' + hh + ':' + mm;
            return `<div class="log-row"><time>${when}</time>
              <span class="lt"><span class="mono">${e.kind}</span>${e.device_id ? ' · плата #' + e.device_id : ''}
                <span class="lg-badge ${evTone(e.kind)}">${evLabel(e.kind)}</span></span></div>`;
          }).join('')
        : '<div class="adm-empty">Пусто</div>';
    } catch (e) {
      $('#adm-pending').innerHTML = '<div style="font-size:12px;color:var(--muted)">' + e.message + '</div>';
    }
  }

  /* ---------- 5. добавление машины по коду ---------- */
  function addClaimScreen() {
    const screens = $('#screens');
    if (!screens || $('#screen-claim')) return;
    const sec = document.createElement('section');
    sec.className = 'screen';
    sec.id = 'screen-claim';
    sec.innerHTML = `
      <div class="card" style="margin-top:8px">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Добавить машину</div></div>
        <div style="font-size:13px;color:var(--muted);margin-bottom:12px">
          Включите плату, откройте её страницу и посмотрите «Код привязки».
          Введите его здесь — машина появится в аккаунте.
        </div>
        <input id="claim-code" placeholder="XXXX-XXXX-XXXX-XXXX" autocapitalize="characters"
          style="width:100%;padding:14px;border-radius:10px;border:1px solid var(--line);
                 background:var(--surface);color:var(--text);font-size:18px;letter-spacing:1px;text-align:center">
        <button id="claim-go" style="margin-top:12px;width:100%;padding:12px;border:0;border-radius:10px;
          background:var(--accent);color:#fff;font-weight:600">Привязать</button>
        <div id="claim-hint" style="font-size:13px;color:var(--muted);margin-top:10px;min-height:18px"></div>
      </div>`;
    screens.insertBefore(sec, $('#screen-more') || null);
  }

  function showClaim() {
    styleSubScreens();
    $$('.screen').forEach(s => s.classList.remove('active'));
    const sec = $('#screen-claim');
    if (sec) sec.classList.add('active');
    $$('.tab').forEach(t => t.classList.remove('active'));
    const sc = $('#screens'); if (sc) sc.scrollTop = 0;
    const input = $('#claim-code');
    if (input) input.value = '';
    const h = $('#claim-hint');
    if (h) h.textContent = '';
    if (input) setTimeout(() => input.focus(), 100);
  }

  function bindClaimButton() {
    const btn = $('#claim-go');
    if (!btn || btn.dataset.lcBound === '1') return;
    btn.dataset.lcBound = '1';
    btn.addEventListener('click', async (e) => {
      e.preventDefault();
      const code = ($('#claim-code').value || '').trim();
      const h = $('#claim-hint');
      if (!code) return;
      btn.disabled = true;
      h.textContent = 'Привязываю…';
      try {
        await api('/api/claim', { code });
        h.textContent = 'Готово. Машина добавлена.';
        setTimeout(() => location.reload(), 900);
      } catch (err) {
        h.textContent = err.message;
        btn.disabled = false;
      }
    }, true);
  }


  /* ---------- 6. обновление прошивки ---------- */
  function addFirmwareScreen() {
    const screens = $('#screens');
    if (!screens || $('#screen-fw')) return;
    const sec = document.createElement('section');
    sec.className = 'screen';
    sec.id = 'screen-fw';
    sec.innerHTML = `
      <div class="card" style="margin-top:8px">
        <div class="card-header"><div class="accent-line"></div><div style="font-size:16px">Обновление прошивки</div></div>
        <div style="font-size:13px;color:var(--muted);margin-bottom:10px">
          Файл скачивается на плату с сервера и прошивается. Плата перезагрузится,
          настройки и привязка сохранятся.
        </div>
        <div id="fw-list"></div>
        <div id="fw-hint" style="font-size:12px;color:var(--muted);margin-top:10px;min-height:18px"></div>
      </div>`;
    screens.insertBefore(sec, $('#screen-more') || null);
  }

  function showFirmware() {
    styleSubScreens();
    if (userRole !== 'admin') { toast('Обновление делает администратор'); return; }
    $$('.screen').forEach(s => s.classList.remove('active'));
    const sec = $('#screen-fw');
    if (sec) sec.classList.add('active');
    $$('.tab').forEach(t => t.classList.remove('active'));
    const sc = $('#screens'); if (sc) sc.scrollTop = 0;
    loadFirmware();
  }

  async function loadFirmware() {
    const box = $('#fw-list');
    const hint = $('#fw-hint');
    if (!box) return;
    box.innerHTML = '<div style="font-size:13px;color:var(--muted)">Читаю список…</div>';
    try {
      const r = await api('/api/admin/firmware');
      const items = r.items || [];
      if (!items.length) {
        box.innerHTML = '<div style="font-size:13px;color:var(--muted)">На сервере нет файлов прошивки.</div>';
        return;
      }
      box.innerHTML = items.map(it => `
        <div style="display:flex;align-items:center;gap:10px;padding:10px 0;border-bottom:1px solid var(--line)">
          <div style="flex:1">
            <a href="${it.url}" target="_blank" rel="noopener"
               style="color:var(--accent);text-decoration:none">${it.name}</a>
            <div style="font-size:12px;color:var(--muted)">${(it.size / 1048576).toFixed(2)} МБ · нажмите имя, чтобы скачать</div></div>
          <button data-fw="${it.name}" style="border:0;border-radius:8px;background:var(--accent);color:#fff;padding:8px 12px">Установить</button>
        </div>`).join('');
      $$('#fw-list [data-fw]').forEach(b => b.onclick = () => installFirmware(b.dataset.fw));
      hint.textContent = '';
    } catch (e) {
      box.innerHTML = '<div style="font-size:12px;color:var(--muted)">' + e.message + '</div>';
    }
  }

  async function installFirmware(file) {
    const hint = $('#fw-hint');
    if (!confirm('Прошить плату файлом ' + file + '?')) return;
    try {
      const devs = await api('/api/devices');
      const list = devs.devices || [];
      const dev = list.find(d => d.online) || list[0];
      if (!dev) throw new Error('нет устройств');
      const r = await api('/api/admin/firmware/update', { device: dev.id, file });
      hint.textContent = r.pushed
        ? 'Команда отправлена. Плата скачает файл и перезагрузится.'
        : 'Плата офлайн — команда в очереди, начнётся при связи.';
    } catch (e) { hint.textContent = e.message; }
  }

  /* ---------- индикатор связи с платой ---------- */
  // Живёт в шапке, рядом с кнопками: ничего не перекрывает ни на телефоне,
  // ни на компьютере. На узком экране — только точка и короткий текст,
  // полная строка (возраст и версия прошивки) — в подсказке и на широком.
  function addLinkBadge() {
    if (document.getElementById('link-badge')) return;
    // На узких экранах штатный «Online» дублирует наш бейдж и не помещается
    // рядом с ним в шапке — оставляем один индикатор с версией прошивки.
    if (!document.getElementById('link-badge-style')) {
      const style = document.createElement('style');
      style.id = 'link-badge-style';
      style.textContent = '@media (max-width: 900px){#conn-badge{display:none!important}}' +
        '#link-badge{max-width:46vw}@media (min-width:720px){#link-badge{max-width:none}}';
      document.head.appendChild(style);
    }
    const host = document.querySelector('.appbar .actions') || document.querySelector('.appbar');
    if (!host) return;

    const b = document.createElement('div');
    b.id = 'link-badge';
    b.style.cssText = 'display:flex;align-items:center;gap:6px;font:11px/1.1 -apple-system,Segoe UI,Roboto,Arial;' +
      'padding:5px 9px;border-radius:999px;background:rgba(120,120,128,.14);color:var(--muted);' +
      'white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:42vw;flex:0 1 auto;margin-right:4px';
    const dot = document.createElement('span');
    dot.style.cssText = 'width:7px;height:7px;border-radius:50%;background:currentColor;flex:0 0 auto';
    b.appendChild(dot);
    const label = document.createElement('span');
    label.style.cssText = 'overflow:hidden;text-overflow:ellipsis';
    b.appendChild(label);
    host.insertBefore(b, host.firstChild);

    const human = (s) => s < 60 ? (s + ' с') : (s < 3600 ? (Math.round(s / 60) + ' мин') : (Math.round(s / 3600) + ' ч'));

    async function tick() {
      const wide = window.innerWidth >= 720;
      try {
        // Показываем выбранную машину, а не самую свежую по связи:
        // /api/status отвечает данными именно текущего устройства.
        const r = await api('/api/status');
        const on = !!r.online;
        const age = (typeof r.age === 'number' && r.age >= 0) ? r.age : -1;
        const fw = r.fw || '';
        const who = r.device_name || r.device || 'плата';
        if (on) {
          label.textContent = wide ? (who + ' · на связи · ' + (age >= 0 ? human(age) + ' назад' : '') + (fw ? ' · ' + fw : ''))
                                   : (who + ' · на связи');
          b.style.color = 'var(--accent)';
        } else {
          label.textContent = wide ? (who + ' · нет связи' + (age >= 0 ? ' · ' + human(age) : '') + (fw ? ' · ' + fw : ''))
                                   : (who + ' · нет связи');
          b.style.color = '#d9534f';
        }
        b.title = who + ' — текущая машина: ' +
          (on ? 'плата на связи' : 'плата не на связи') +
          (age >= 0 ? ', ' + human(age) + ' назад' : '') +
          (fw ? ', прошивка ' + fw : '');
      } catch (e) {
        label.textContent = wide ? 'сервер недоступен' : 'нет сервера';
        b.style.color = '#d9534f';
        b.title = 'Сервер не отвечает';
      }
    }
    tick();
    setInterval(tick, 20000);
    window.addEventListener('resize', () => { tick(); });
  }


  /* ---------- выбор машины ---------- */
  function addDeviceChip() {
    if (document.getElementById('dev-chip')) return;
    const host = document.querySelector('.appbar .actions');
    if (!host) return;
    const b = document.createElement('button');
    b.id = 'dev-chip';
    b.style.cssText = 'display:flex;align-items:center;gap:4px;border:0;background:rgba(120,120,128,.14);' +
      'color:var(--text);border-radius:999px;padding:5px 10px;font:12px/1.1 -apple-system,Segoe UI,Roboto;' +
      'max-width:34vw;overflow:hidden;white-space:nowrap;margin-right:4px';
    b.innerHTML = '<span id="dev-chip-name" style="overflow:hidden;text-overflow:ellipsis">Машина</span>' +
      '<svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2.4"><path d="M6 9l6 6 6-6"/></svg>';
    b.onclick = openDeviceSheet;
    host.insertBefore(b, host.firstChild);
    refreshDeviceChip();
    setInterval(refreshDeviceChip, 30000);
  }

  async function refreshDeviceChip() {
    try {
      const j = await api('/api/status');
      const el = document.getElementById('dev-chip-name');
      if (el) el.textContent = j.device_name || j.device || 'Машина';
      if (j && j.device_id) window.__leafDeviceId = j.device_id;
    } catch (e) { }
  }

  async function openDeviceSheet() {
    let list = [];
    try { list = (await api('/api/devices')).devices || []; } catch (e) { }
    const cur = window.__leafDeviceId;
    const ov = document.createElement('div');
    ov.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,.45);z-index:10000;' +
      'display:flex;align-items:flex-end;justify-content:center';
    const sheet = document.createElement('div');
    sheet.style.cssText = 'background:var(--surface);color:var(--text);width:100%;max-width:540px;' +
      'border-radius:16px 16px 0 0;padding:16px;max-height:72vh;overflow:auto';
    const ago = (s) => {
      if (!s) return 'нет данных';
      const d = Math.max(0, Math.round(Date.now() / 1000 - s));
      if (d < 90) return 'на связи';
      if (d < 3600) return Math.round(d / 60) + ' мин назад';
      if (d < 86400) return Math.round(d / 3600) + ' ч назад';
      return Math.round(d / 86400) + ' дн назад';
    };
    sheet.innerHTML = '<div style="font-weight:600;margin-bottom:10px">Выбор машины</div>' +
      (list.length ? list.map(d => `
        <div data-dev="${d.id}" style="display:flex;align-items:center;gap:10px;padding:12px 0;border-bottom:1px solid var(--line);cursor:pointer">
          <i style="width:8px;height:8px;border-radius:50%;background:${d.online ? 'var(--accent)' : 'var(--muted)'}"></i>
          <div style="flex:1">
            <div style="font-size:14px">${d.name || d.login}${String(d.id) === String(cur) ? ' ✓' : ''}</div>
            <div style="font-size:12px;color:var(--muted)">${d.login} · ${d.fw || ''} · ${ago(d.last_seen)}</div>
          </div>
        </div>`).join('') : '<div style="font-size:13px;color:var(--muted)">Машин нет</div>') +
      `<div data-add="1" style="padding:14px 0;color:var(--accent);font-size:14px;cursor:pointer">+ Добавить машину по коду</div>
       <button id="dev-cancel" style="margin-top:10px;width:100%;padding:12px;border:0;border-radius:10px;background:var(--surface-2);color:var(--text)">Отмена</button>`;
    ov.appendChild(sheet);
    document.body.appendChild(ov);
    ov.onclick = (e) => { if (e.target === ov) ov.remove(); };
    sheet.querySelector('#dev-cancel').onclick = () => ov.remove();
    sheet.querySelectorAll('[data-dev]').forEach(el => el.onclick = async () => {
      try {
        await api('/api/device/select', { device: el.dataset.dev });
        ov.remove();
        location.reload();
      } catch (e) { alert(e.message); }
    });
    const add = sheet.querySelector('[data-add]');
    if (add) add.onclick = () => { ov.remove(); showClaim(); };
  }

  /* ---------- запуск ---------- */
  function init() {
    cleanMore();
    addScreen();
    addTab();
    addSettingsScreen();
    addAdminScreen();
    addClaimScreen();
    addFirmwareScreen();
    addLinkBadge();
    addDeviceChip();
    interceptMore();
    bindCards();
    initDesignSwitch();
  loadRole().then(() => { addMoreRows(); showNotices(); styleSubScreens(); });
  initGarageLimit();
  initClimPlan();
    const obs = new MutationObserver(() => {
      cleanMore(); interceptMore(); bindCards(); addMoreRows(); bindClaimButton();
    });
    obs.observe(document.body, { childList: true, subtree: true });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
