/* Leaf Control · RADIUS (бета): живой интерфейс поверх старого дизайна.
   Все данные и команды идут через сервер; статус опрашивается каждые 3 с. */
(function () {
  'use strict';

  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const esc = (v) => String(v === null || v === undefined ? '' : v)
    .replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const f1 = (v, d) => (v === null || v === undefined || !isFinite(Number(v))) ? '—'
    : Number(v).toFixed(d === undefined ? 1 : d).replace('.', ',');
  const int = (v) => (v === null || v === undefined || !isFinite(Number(v))) ? '—'
    : Math.round(Number(v)).toLocaleString('ru-RU');
  const clock = (ts) => {
    if (!ts) return '—';
    const d = new Date(Number(ts) * 1000);
    return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
  };
  const day = (ts) => {
    if (!ts) return '—';
    return new Date(Number(ts) * 1000).toLocaleDateString('ru-RU', { day: 'numeric', month: 'short' });
  };
  const when = (ts) => {
    if (!ts) return '—';
    return new Date(Number(ts) * 1000).toLocaleString('ru-RU',
      { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });
  };
  const dur = (s) => {
    s = Math.max(0, Math.round(Number(s) || 0));
    if (s < 60) return s + ' с';
    if (s < 3600) return Math.floor(s / 60) + ' мин';
    return Math.floor(s / 3600) + ' ч ' + String(Math.floor((s % 3600) / 60)).padStart(2, '0') + ' мин';
  };

  async function api(path, body, method) {
    const opt = { credentials: 'same-origin', cache: 'no-store' };
    opt.method = method || (body !== undefined ? 'POST' : 'GET');
    if (body !== undefined) {
      opt.headers = { 'Content-Type': 'application/json' };
      opt.body = JSON.stringify(body);
    }
    const r = await fetch(path, opt);
    let j = null;
    try { j = await r.json(); } catch (e) { /* пустой ответ */ }
    if (!r.ok) {
      const err = new Error((j && j.error) || ('ошибка ' + r.status));
      err.status = r.status;
      throw err;
    }
    return j || {};
  }

  let toastTimer = null;
  function toast(msg) {
    const t = $('#toast');
    if (!t) return;
    t.textContent = msg;
    t.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.remove('show'), 3200);
  }

  /* ---------- состояние ---------- */
  const S = {
    soc: null, km: null, amb: null, cabin: null, charging: false, hvac: false,
    lock: null, lights: false, vent: false, trunk: false, power: 0, v12: null,
    odo: null, gids: null, soh: null, chargeLimit: null, setpoint: 22
  };
  const DATA = {
    status: null, stats: null, sessions: [], devices: [], config: {},
    settings: {}, sched: [], fw: []
  };
  const PERIODS = [
    { d: 1, label: '24 часа' }, { d: 7, label: 'неделя' },
    { d: 30, label: 'месяц' }, { d: 0, label: 'всё время' }
  ];
  let period = 7;
  let statSub = 'charges';
  let busyCmd = false;

  function deviceParam() {
    const id = DATA.status && DATA.status.device_id;
    return id ? 'device=' + encodeURIComponent(id) + '&' : '';
  }

  /* ---------- тема ---------- */
  function setTheme(mode) {
    document.documentElement.dataset.theme = mode;
    try { localStorage.setItem('radius-theme', mode); } catch (e) { /* приватный режим */ }
    const b = $('#btnTheme');
    if (b) b.innerHTML = mode === 'dark'
      ? '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg>'
      : '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="4.5"/><path d="M12 2.5v3M12 18.5v3M2.5 12h3M18.5 12h3M5 5l2.1 2.1M16.9 16.9 19 19M19 5l-2.1 2.1M7.1 16.9 5 19"/></svg>';
  }

  /* ---------- статус ---------- */
  function applyStatus(j) {
    DATA.status = j || {};
    S.soc = (j.soc !== undefined && j.soc >= 0) ? j.soc : j.isoc;
    S.km = (j.irange !== undefined && j.irange >= 0) ? j.irange : null;
    S.amb = j.amb;
    S.cabin = j.cabin;
    S.charging = !!j.charging;
    S.hvac = !!j.hvac;
    S.vent = !!j.vent;
    S.lock = j.lock;
    S.lights = (j.lights === 1);
    S.power = Number(j.power) || 0;
    S.v12 = j.uds && j.uds.v12;
    S.odo = j.odo;
    S.gids = j.gids;
    S.soh = j.soh;
    S.setpoint = Number(j.setpoint) || 22;
    sync();
  }

  function sync() {
    const soc = (S.soc === null || S.soc === undefined || S.soc < 0) ? null : Math.round(S.soc);
    $('#tSoc').innerHTML = (soc === null ? '—' : soc) + '<em>%</em>';
    $('#tmSoc').textContent = (soc === null ? '—' : soc + '%');
    $('#tmBar').style.width = (soc === null ? 0 : Math.max(0, Math.min(100, soc))) + '%';
    $('#tmKm').innerHTML = (S.km === null ? '—' : int(S.km)) + '<em>км</em>';
    $('#tKm').innerHTML = (S.km === null ? '—' : int(S.km)) + '<em>км</em>';
    $('#tAmb').innerHTML = (S.amb === null || S.amb === undefined ? '—' :
      (S.amb > 0 ? '+' : '') + f1(S.amb)) + '<em>°</em>';
    $('#tCabin').innerHTML = (S.cabin === null || S.cabin === undefined ? '—' :
      (S.cabin > 0 ? '+' : '') + f1(S.cabin)) + '<em>°</em>';
    $('#tmPwr').innerHTML = f1(Math.abs(S.power)) + '<em>кВт</em>';
    $('#tmV12').innerHTML = (S.v12 ? f1(S.v12, 2) : '—') + '<em>В</em>';
    $('#tmOdo').innerHTML = (S.odo ? int(S.odo) : '—') + '<em>км</em>';
    $('#tmGids').textContent = (S.gids === null || S.gids === undefined || S.gids < 0) ? '—' : int(S.gids);
    $('#tmBattHint').textContent = 'батарея' + (S.chargeLimit ? ' · лимит ' + S.chargeLimit + '%' : '');
    $('#tmLimit').style.left = (S.chargeLimit ? S.chargeLimit : 90) + '%';
    $('#clTemp').textContent = f1(S.setpoint);
    const btn = $('#clBtn');
    btn.textContent = S.hvac ? 'Выключить климат' : 'Включить климат';
    btn.classList.toggle('stop', S.hvac);

    let state = 'стоянка';
    if (S.charging) state = 'зарядка · ' + f1(Math.abs(S.power)) + ' кВт';
    else if (S.hvac) state = 'работает климат';
    else if (S.vent) state = 'вентиляция';
    else if (DATA.status && DATA.status.on === 1) state = 'в движении';
    $('#pillState').textContent = state;

    const online = DATA.status && (DATA.status.online || ((Date.now() / 1000) - (DATA.status.age || 0)) < 120);
    $('#connText').textContent = online
      ? 'онлайн · ' + (DATA.status.link || 'связь') + ' · ' + (DATA.status.age ? Math.round(DATA.status.age) + ' с назад' : 'сейчас')
      : 'нет связи с платой';
    const dn = DATA.status && DATA.status.device_name;
    $('#sceneTitle').textContent = dn || 'Leaf Control';
    $('#btnDevice').textContent = dn || 'устройство…';

    $$('.orbit-btn').forEach((b) => {
      const a = b.dataset.act;
      if (a === 'lock') {
        b.classList.toggle('on', S.lock === 0);
        const l = b.querySelector('[data-lbl="lock"]');
        if (l) l.textContent = S.lock === 1 ? 'Замок' : (S.lock === 0 ? 'Открыт' : 'Замок');
      }
      if (a === 'climate') {
        b.classList.toggle('on', S.hvac);
        const l = b.querySelector('[data-lbl="climate"]');
        if (l) l.textContent = S.hvac ? 'Климат ✓' : 'Климат';
      }
      if (a === 'charge') b.classList.toggle('on', S.charging);
      if (a === 'lights') b.classList.toggle('on', S.lights);
    });
    $$('.hs').forEach((h) => {
      const a = h.dataset.act;
      if (a === 'lock') h.classList.toggle('on', S.lock === 0);
      if (a === 'charge') h.classList.toggle('on', S.charging);
      if (a === 'lights') h.classList.toggle('on', S.lights);
    });
  }

  /* ---------- команды ---------- */
  async function sendCmd(path, body) {
    if (busyCmd) { toast('Дождитесь завершения предыдущей команды'); return; }
    busyCmd = true;
    try {
      const r = await api(path, body, 'POST');
      toast(r && r.pushed ? 'Команда отправлена на плату' : 'Команда в очереди — плата примет при связи');
      setTimeout(pollNow, 1500);
    } catch (e) {
      toast(e.message || 'Не удалось отправить команду');
    } finally {
      busyCmd = false;
    }
  }
  function run(act) {
    switch (act) {
      case 'lock': return sendCmd('/lock');
      case 'unlock': return sendCmd('/unlock');
      case 'charge': return sendCmd(S.charging ? '/charge_off' : '/charge_on');
      case 'refresh': return sendCmd('/refresh');
      case 'lights': return sendCmd('/lights');
      case 'horn': return sendCmd('/hornlights');
      case 'climate':
        return sendCmd(S.hvac ? '/hvac_off' :
          '/hvac_on?setpoint=' + encodeURIComponent(S.setpoint) +
          '&minutes=' + encodeURIComponent(Math.max(5, Math.min(60, Number($('#clMinutes').value) || 15))));
      case 'vent':
        toast('Отдельной команды вентиляции нет — используйте климат');
        return;
      case 'trunk':
        toast('Управление багажником платой не поддерживается');
        return;
      default: return;
    }
  }

  /* ---------- шторка и веер ---------- */
  const sheet = $('#sheet');
  function openSheet(pane) {
    sheet.classList.add('open');
    sheet.setAttribute('aria-hidden', 'false');
    if (pane) selectPane(pane);
    closeDock();
  }
  function closeSheet() {
    sheet.classList.remove('open');
    sheet.setAttribute('aria-hidden', 'true');
  }
  function selectPane(name) {
    $$('.stab').forEach((b) => b.classList.toggle('on', b.dataset.pane === name));
    $$('.spane').forEach((p) => p.classList.toggle('on', p.dataset.pane === name));
    const titles = { trips: 'Поездки', battery: 'Батарея', charge: 'Зарядки',
      climate: 'Климат', sys: 'Система', stats: 'Статистика' };
    $('#sTitle').textContent = titles[name] || 'Данные';
    $('.sbody').scrollTop = 0;
    if (name === 'stats') renderStats();
    if (name === 'sys') refreshSys();
  }
  function openDock() { $('#dock').classList.add('open'); $('#veil').classList.add('on'); }
  function closeDock() { $('#dock').classList.remove('open'); $('#veil').classList.remove('on'); }

  function bindHold(sel, after) {
    $$(sel).forEach((btn) => {
      let timer = null, fired = false;
      const start = (e) => {
        if (e.button !== undefined && e.button !== 0) return;
        fired = false;
        btn.classList.add('holding');
        timer = setTimeout(() => { fired = true; btn.classList.remove('holding'); after(btn); }, 900);
      };
      const cancel = () => { clearTimeout(timer); timer = null; btn.classList.remove('holding'); };
      btn.addEventListener('pointerdown', start);
      btn.addEventListener('pointerup', cancel);
      btn.addEventListener('pointerleave', cancel);
      btn.addEventListener('pointercancel', cancel);
      btn.addEventListener('click', (e) => { if (!fired) e.preventDefault(); });
      btn.addEventListener('keydown', (e) => {
        if ((e.key === 'Enter' || e.key === ' ') && !timer) { e.preventDefault(); start({}); }
      });
      btn.addEventListener('keyup', cancel);
    });
  }
  function fitDock() {
    const w = window.innerWidth;
    const r = w < 640 ? 132 : (w < 980 ? 124 : 172);
    $$('.orbit-btn').forEach((b) => b.style.setProperty('--r', r + 'px'));
  }

  /* ---------- карточки ---------- */
  function card(title, badge, body, hint) {
    return '<div class="scard"><div class="top-row"><b class="t">' + esc(title) + '</b>' +
      (badge ? '<span class="badge">' + badge + '</span>' : '') + '</div>' + body +
      (hint ? '<div class="hint">' + hint + '</div>' : '') + '</div>';
  }
  function kv(pairs) {
    return pairs.map((p) => '<div class="kv"><span>' + esc(p[0]) + '</span><b>' + p[1] + '</b></div>').join('');
  }
  function bar(pct) {
    const v = Math.max(0, Math.min(100, Number(pct) || 0));
    return '<div class="bar"><i style="width:' + v + '%"></i></div>';
  }
  function chart(rows, key, altKey) {
    if (!rows || !rows.length) return '<div class="hint">Нет данных за период</div>';
    const max = Math.max(1, ...rows.map((r) => Number(r[key]) || 0));
    return '<div class="chart">' + rows.map((r) => {
      const h = Math.max(2, Math.round((Number(r[key]) || 0) / max * 100));
      const a = altKey ? Math.max(2, Math.round((Number(r[altKey]) || 0) / max * 100)) : null;
      return '<div class="col"><i style="height:' + h + '%"></i>' +
        (a !== null ? '<i class="alt" style="height:' + a + '%"></i>' : '') +
        '<span>' + esc(String(r.date || '').slice(8, 10)) + '</span></div>';
    }).join('') + '</div><div class="legend"><span><i></i>км</span>' +
      (altKey ? '<span><i class="g"></i>кВт·ч</span>' : '') + '</div>';
  }

  /* ---------- данные ---------- */
  async function loadPeriod(days) {
    period = days;
    try {
      const [st, se] = await Promise.all([
        api('/api/stats?' + deviceParam() + 'days=' + days),
        api('/api/sessions?' + deviceParam() + 'days=' + (days || 365))
      ]);
      DATA.stats = st;
      DATA.sessions = (se.sessions || []).slice().sort((a, b) => (b.started_at || 0) - (a.started_at || 0));
      renderTrips();
      renderBattery();
      renderCharge();
      renderStats();
    } catch (e) {
      toast('Не удалось загрузить историю: ' + e.message);
    }
  }

  async function loadMeta() {
    const jobs = [
      ['/api/config', 'config'], ['/api/settings', 'settings'],
      ['/api/sched', 'sched'], ['/api/devices', 'devices']
    ];
    for (const [url, key] of jobs) {
      try { DATA[key] = await api(url + (key === 'config' || key === 'sched' ? '?' + deviceParam() : '')); }
      catch (e) { /* часть настроек может быть недоступна */ }
    }
    const cfg = DATA.config.config || {};
    S.chargeLimit = Number(cfg.charge_limit) || null;
    sync();
    renderCharge();
    renderClimate();
    renderSys();
  }

  async function loadFirmwareList() {
    try {
      const j = await api('/api/admin/firmware');
      DATA.fw = j.items || [];
    } catch (e) { DATA.fw = []; }
    const box = $('#sysFw');
    if (!box) return;
    const installed = DATA.status && DATA.status.fw;
    box.innerHTML = card('Установлено', null, kv([
      ['Прошивка', esc(installed || '—')],
      ['Описание', esc((DATA.status && DATA.status.fw_desc) || '—')],
      ['Плата', esc((DATA.status && DATA.status.device_name) || '—')]
    ])) + '<div class="hint">Доступные файлы: ' +
      (DATA.fw.length ? DATA.fw.map((x) => esc(x.name)).join(', ') : 'нет') +
      '. Обновление — в админке или через /api/admin/firmware/update.</div>';
  }

  /* ---------- рендер: поездки ---------- */
  function renderTrips() {
    const st = DATA.stats || {};
    const t = st.trips || {};
    const list = DATA.sessions.filter((r) => r.kind === 'trip');
    $('#tripsTitle').textContent = 'поездки · ' + (t.count || list.length) + ' · ' + f1(t.km || 0, 0) + ' км';
    $('#tripsTotals').innerHTML = kv([
      ['Поездок', esc(t.count || list.length)],
      ['Пробег', f1(t.km || 0) + ' км'],
      ['Энергия', f1(t.kwh || 0) + ' кВт·ч'],
      ['Расход', (st.consumption_km_per_kwh ? f1(st.consumption_km_per_kwh) + ' км/кВт·ч' : '—')]
    ]);
    $('#tripsChart').innerHTML = chart(st.per_day || [], 'km', 'kwh');
    $('#tripsList').innerHTML = list.length ? list.slice(0, 40).map((r) =>
      card((day(r.started_at) || 'поездка') + ' · ' + clock(r.started_at) + ' → ' + clock(r.ended_at),
        f1(r.distance_km || 0, 0) + ' км',
        '<div class="meta"><span><b>' + f1(r.energy_kwh || 0) + '</b> кВт·ч</span>' +
        '<span><b>' + dur(r.duration_s) + '</b></span>' +
        '<span>SOC <b>' + f1(r.soc_start, 0) + '→' + f1(r.soc_end, 0) + '</b></span></div>')
    ).join('') : '<div class="scard hint">За этот период поездок нет</div>';
  }

  /* ---------- рендер: батарея ---------- */
  function renderBattery() {
    const st = DATA.stats || {};
    $('#batteryCards').innerHTML =
      card('Заряд и запас', (S.chargeLimit ? 'лимит ' + S.chargeLimit + '%' : null),
        bar(S.soc || 0) + '<div class="meta"><span>SOC <b>' + f1(S.soc, 0) + '%</b></span>' +
        '<span>запас <b>' + (S.km === null ? '—' : int(S.km)) + ' км</b></span>' +
        '<span>SOH <b>' + f1(st.soh_current || S.soh, 0) + '%</b></span></div>') +
      card('Ёмкость и состояние', null, kv([
        ['SOH', f1(st.soh_current || S.soh, 0) + '%'],
        ['Напряжение', (DATA.status && DATA.status.v ? f1(DATA.status.v, 1) + ' В' : '—')],
        ['Ток', (DATA.status && DATA.status.i !== undefined ? f1(DATA.status.i, 1) + ' А' : '—')],
        ['Температура батареи', (DATA.status && DATA.status.btemp !== undefined && DATA.status.btemp > -90 ? f1(DATA.status.btemp) + ' °C' : '—')],
        ['SIM за месяц', (st.sim_mb !== undefined ? f1(st.sim_mb, 0) + ' МБ' : '—')]
      ]));
    $('#batteryChart').innerHTML = chart(st.per_day || [], 'km', 'kwh');
    $('#batteryCells').innerHTML = kv([
      ['Разброс ячеек', (DATA.status && DATA.status.cdelta >= 0 ? f1(DATA.status.cdelta, 0) + ' мВ' : '—')],
      ['Температура батареи', (DATA.status && DATA.status.btemp > -90 ? f1(DATA.status.btemp) + ' °C' : '—')],
      ['GIDS', (S.gids === null || S.gids === undefined || S.gids < 0 ? '—' : int(S.gids))],
      ['Бортовая сеть', (S.v12 ? f1(S.v12, 2) + ' В' : '—')],
      ['Пробег', (S.odo ? int(S.odo) + ' км' : '—')],
      ['Прошивка', esc((DATA.status && DATA.status.fw) || '—')]
    ]);
  }

  /* ---------- рендер: зарядки ---------- */
  function renderCharge() {
    const st = DATA.stats || {};
    const c = st.charges || {};
    const list = DATA.sessions.filter((r) => r.kind === 'charge');
    $('#chargeNow').innerHTML = card(
      S.charging ? 'Идёт зарядка · ' + f1(Math.abs(S.power)) + ' кВт' : 'Кабель не подключён',
      S.charging ? 'идёт' : 'ожидание',
      kv([
        ['Сейчас', f1(S.soc, 0) + '%'],
        ['Лимит', (S.chargeLimit ? S.chargeLimit + '%' : 'выключен')],
        ['Мощность', (S.charging ? f1(Math.abs(S.power)) + ' кВт' : '—')],
        ['Tariff', esc((DATA.status && DATA.status.tariff_night !== undefined ? DATA.status.tariff_night : '—')) + ' ₽/кВт·ч ночь']
      ]));
    $('#chargeList').innerHTML = list.length ? list.slice(0, 40).map((r) =>
      card(day(r.started_at) + ' · ' + clock(r.started_at) + ' → ' + clock(r.ended_at),
        '+' + f1((Number(r.soc_end) || 0) - (Number(r.soc_start) || 0), 0) + '%',
        '<div class="meta"><span><b>' + f1(r.energy_kwh || 0) + '</b> кВт·ч</span>' +
        '<span><b>' + dur(r.duration_s) + '</b></span>' +
        '<span>SOC <b>' + f1(r.soc_start, 0) + '→' + f1(r.soc_end, 0) + '</b></span></div>' +
        bar((Number(r.soc_end) || 0) - (Number(r.soc_start) || 0)))
    ).join('') : '<div class="scard hint">Зарядок за период нет</div>';
    const cost = st.cost || {};
    $('#chargeTariff').innerHTML = card('Расходы и экономия', st.settings && st.settings.currency, kv([
      ['Зарядки', (c.count || 0) + ' · ' + f1(c.kwh || 0) + ' кВт·ч'],
      ['Электроэнергия', f1(cost.electric || 0) + ' ' + esc(cost.currency || '')],
      ['Бензин', f1(cost.petrol || 0) + ' ' + esc(cost.currency || '')],
      ['Экономия', f1(cost.saved || 0) + ' ' + esc(cost.currency || '')]
    ]));
  }

  /* ---------- рендер: климат ---------- */
  function renderClimate() {
    $('#clSensors').innerHTML = kv([
      ['Салон', (S.cabin !== null && S.cabin !== undefined ? f1(S.cabin) + ' °C' : '—')],
      ['Улица', (S.amb !== null && S.amb !== undefined ? f1(S.amb) + ' °C' : '—')],
      ['Цель', f1(S.setpoint) + ' °C'],
      ['Вентилятор', String(DATA.status && DATA.status.fan !== undefined ? DATA.status.fan : '—')],
      ['Климат', S.hvac ? 'включён' : 'выключен'],
      ['Остаток', (DATA.status && DATA.status.hvac_timer ? dur(DATA.status.hvac_timer * 60) : '—')]
    ]);
    const hist = DATA.sessions.filter((r) => r.kind === 'climate');
    $('#clHistory').innerHTML = hist.length ? hist.slice(0, 20).map((r) =>
      card(day(r.started_at) + ' ' + clock(r.started_at), dur(r.duration_s),
        '<div class="meta"><span>' + f1(r.energy_kwh || 0) + ' кВт·ч</span></div>')
    ).join('') : '<div class="scard hint">История климата появится после поездок с климатом</div>';
    renderSched();
  }

  function renderSched() {
    const rows = (DATA.sched.schedules || []).slice().sort((a, b) => (a.at_ts || 0) - (b.at_ts || 0));
    $('#clPlanList').innerHTML = rows.length ? rows.slice(0, 10).map((x) => {
      let minutes = 0;
      try { minutes = Number(JSON.parse(x.args || '{}').minutes) || 0; } catch (e) { /* пусто */ }
      return '<div class="kv"><span>' + when(x.at_ts) + (minutes ? ' · ' + minutes + ' мин' : '') +
        '</span><b>' + esc(x.status || '') +
        ' <button data-cancel="' + esc(x.id) + '" style="border:0;background:none;color:var(--bad);cursor:pointer">×</button></b></div>';
    }).join('') : '<div class="hint">Нет запланированных включений</div>';
    $('#clPlanHint').textContent = rows.length ? 'Ближайший запуск: ' + when(rows[0].at_ts) : '';
    $$('#clPlanList [data-cancel]').forEach((b) => {
      b.onclick = async () => {
        try { await api('/api/sched/cancel', { id: Number(b.dataset.cancel) }); toast('Отменено'); loadMeta(); }
        catch (e) { toast(e.message); }
      };
    });
  }

  /* ---------- рендер: система ---------- */
  function renderSys() {
    const j = DATA.status || {};
    $('#sysInfo').innerHTML = kv([
      ['Машина', esc(j.device_name || '—')],
      ['Модель', esc(j.device || '—')],
      ['Прошивка', esc(j.fw || '—')],
      ['VIN', esc(j.vin || '—')],
      ['Пробег', (j.odo ? int(j.odo) + ' км' : '—')],
      ['SIM', j.sim_mb !== undefined ? f1(j.sim_mb, 0) + ' МБ' : '—'],
      ['Связь', (j.link || '—') + ' · ' + (j.rssi !== undefined ? j.rssi + ' dBm' : '')],
      ['Последний ответ', (j.last_seen ? when(j.last_seen) : (j.age ? Math.round(j.age) + ' с назад' : '—'))]
    ]);
    const devs = DATA.devices.devices || [];
    $('#sysDevices').innerHTML = devs.length ? devs.map((d) =>
      '<div class="kv"><span>' + esc(d.name || d.login) + (d.online ? ' · онлайн' : '') +
      '</span><b><button data-dev="' + esc(d.id) + '" style="border:0;background:none;color:var(--acc);cursor:pointer">' +
      (String(j.device_id) === String(d.id) ? 'текущая' : 'выбрать') + '</button></b></div>'
    ).join('') : '<div class="hint">Устройств нет</div>';
    $$('#sysDevices [data-dev]').forEach((b) => {
      b.onclick = async () => {
        try {
          await api('/api/device/select', { device: Number(b.dataset.dev) });
          toast('Машина выбрана');
          setTimeout(() => location.reload(), 600);
        } catch (e) { toast(e.message); }
      };
    });

    renderConfig();
    renderTariff();
    renderMaint();
    renderSched();
  }

  const CFG_GROUPS = [
    ['Режим работы', ['remote_mode']],
    ['Зарядка', ['charge_limit', 'sleep', 'sleep_after', 'wake_fast', 'wake_slow']],
    ['Интернет через SIM', ['modem', 'apn', 'apn_user', 'apn_pass', 'sim_limit_mb']],
    ['Плата в сети', ['host_name']],
    ['Wi-Fi', ['ssid', 'password', 'ssid2', 'password2', 'ssid3', 'password3']],
    ['Дополнительно', ['locking', 'uds', 'uds_tpms']]
  ];
  const CFG_LABELS = {
    remote_mode: 'Удалённый режим (1 — сервер всегда на связи)',
    charge_limit: 'Лимит заряда, % (0 — выключен)',
    sleep: 'Глубокий сон', sleep_after: 'Сон через, ч',
    wake_fast: 'Пробуждение каждые, мин', wake_slow: 'После 7 дней, мин',
    modem: 'Модем SIM800', apn: 'APN', apn_user: 'Логин APN', apn_pass: 'Пароль APN',
    sim_limit_mb: 'Лимит SIM, МБ/мес', host_name: 'Имя в сети',
    ssid: 'Wi-Fi 1 (SSID)', password: 'Пароль Wi-Fi 1', ssid2: 'Wi-Fi 2 (SSID)',
    password2: 'Пароль Wi-Fi 2', ssid3: 'Wi-Fi 3 (SSID)', password3: 'Пароль Wi-Fi 3',
    locking: 'Блокировка кнопок', uds: 'Расширенная диагностика', uds_tpms: 'TPMS'
  };
  function renderConfig() {
    const meta = DATA.config || {};
    const cfg = meta.config || {};
    $('#cfgInfo').textContent = 'Логин: ' + (meta.login || '—') + ' · прошивка: ' + (meta.fw || '—') +
      (meta.ts ? ' · получено ' + when(meta.ts) : ' · плата ещё не присылала настройки');
    $('#cfgFields').innerHTML = CFG_GROUPS.map(([title, keys]) => {
      const fields = keys.filter((k) => cfg[k] !== undefined || k === 'charge_limit').map((k) => {
        const v = cfg[k];
        if (typeof v === 'boolean' || (!isNaN(Number(v)) && (Number(v) === 0 || Number(v) === 1) &&
            ['remote_mode', 'sleep', 'modem', 'locking', 'uds', 'uds_tpms'].indexOf(k) >= 0)) {
          const checked = (v === true || Number(v) === 1);
          return '<label class="field">' + esc(CFG_LABELS[k] || k) +
            '<input type="checkbox" data-cfg="' + k + '"' + (checked ? ' checked' : '') + '></label>';
        }
        if (k.indexOf('pass') >= 0) {
          return '<label class="field">' + esc(CFG_LABELS[k] || k) +
            '<input type="password" data-cfg="' + k + '" value="" placeholder="не менять"></label>';
        }
        const numeric = (v !== undefined && v !== '' && !isNaN(Number(v)));
        return '<label class="field">' + esc(CFG_LABELS[k] || k) +
          '<input type="' + (numeric ? 'number' : 'text') + '" data-cfg="' + k + '" value="' + esc(v === undefined ? '' : v) + '"></label>';
      }).join('');
      return fields ? '<div class="cfggroup"><h5>' + esc(title) + '</h5>' + fields + '</div>' : '';
    }).join('');
  }

  function renderTariff() {
    const s = DATA.settings || {};
    const vals = [
      ['tariff_day', 'Дневной тариф, ₽/кВт·ч'], ['tariff_night', 'Ночной тариф, ₽/кВт·ч'],
      ['night_from', 'Ночь с, ч'], ['night_to', 'Ночь до, ч'],
      ['petrol_price', 'Бензин, ₽/л'], ['petrol_l100', 'Расход бензина, л/100 км']
    ];
    $('#sysTariff').innerHTML = vals.map(([k, label]) =>
      '<label class="field">' + label + '<input type="number" step="0.01" data-set="' + k +
      '" value="' + esc(s[k] === undefined ? '' : s[k]) + '"></label>').join('') +
      '<div class="rowbtns"><button id="setSave">Сохранить тарифы</button></div>';
    $('#setSave').onclick = async () => {
      const body = {};
      $$('#sysTariff [data-set]').forEach((el) => { if (el.value !== '') body[el.dataset.set] = Number(el.value); });
      try { await api('/api/settings', body); toast('Тарифы сохранены'); loadMeta(); }
      catch (e) { toast(e.message); }
    };
  }

  function renderMaint() {
    const st = DATA.stats || {};
    const rows = st.maintenance || [];
    $('#sysMaint').innerHTML = (rows.length ? rows.map((x) =>
      '<div class="kv"><span>' + esc(x.title || 'обслуживание') + '</span><b>' +
      (x.due_km ? int(x.due_km) + ' км' : '—') + '</b></div>').join('') :
      '<div class="hint">Напоминаний нет</div>') +
      '<div class="rowbtns"><button id="maintAdd">Добавить</button></div>';
    $('#maintAdd').onclick = async () => {
      const title = prompt('Название обслуживания');
      if (!title) return;
      const km = prompt('Пробег, км (необязательно)', '');
      try {
        await api('/api/maintenance', { device: DATA.status && DATA.status.device_id,
          title, due_km: km ? Number(km) : null });
        toast('Добавлено'); loadPeriod(period);
      } catch (e) { toast(e.message); }
    };
  }

  async function refreshSys() {
    try { DATA.status = await api('/api/status'); applyStatus(DATA.status); } catch (e) { /* тихо */ }
    await loadMeta();
    await loadFirmwareList();
  }

  /* ---------- рендер: статистика ---------- */
  function renderStats() {
    const st = DATA.stats || {};
    $('statSub').innerHTML = statSub === 'charges' ? statCharges(st) :
      statSub === 'trips' ? statTrips(st) : statTotals(st);
  }
  function statCharges(st) {
    const c = st.charges || {};
    const list = DATA.sessions.filter((r) => r.kind === 'charge');
    return card('Зарядки за период', f1(c.kwh || 0) + ' кВт·ч', kv([
      ['Количество', esc(c.count || list.length)],
      ['Энергия', f1(c.kwh || 0) + ' кВт·ч']
    ])) + list.slice(0, 30).map((r) =>
      card(day(r.started_at) + ' ' + clock(r.started_at), '+' + f1((r.soc_end || 0) - (r.soc_start || 0), 0) + '%',
        '<div class="meta"><span>' + f1(r.energy_kwh || 0) + ' кВт·ч</span><span>' + dur(r.duration_s) + '</span></div>')
    ).join('');
  }
  function statTrips(st) {
    const t = st.trips || {};
    return card('Поездки за период', f1(t.km || 0, 0) + ' км', kv([
      ['Количество', esc(t.count || 0)], ['Пробег', f1(t.km || 0) + ' км'],
      ['Энергия', f1(t.kwh || 0) + ' кВт·ч']
    ])) + card('Пробег по дням', null, chart(st.per_day || [], 'km', 'kwh'));
  }
  function statTotals(st) {
    const cost = st.cost || {};
    const cycles = st.discharge_cycles || [];
    return card('SOH', f1(st.soh_current || S.soh, 0) + '%',
      (st.soh || []).length ? chart(st.soh.map((x) => ({ date: new Date(x.ts * 1000).toISOString(), soh: x.soh })), 'soh') :
      '<div class="hint">История SOH появится после нескольких дней телеметрии</div>') +
      card('Экономия', esc(cost.currency || ''), kv([
        ['Электроэнергия', f1(cost.electric || 0)], ['Бензин', f1(cost.petrol || 0)],
        ['Экономия', f1(cost.saved || 0)]
      ])) +
      card('Разряд после зарядки', null, cycles.length ? cycles.slice(0, 20).map((x) =>
        '<div class="kv"><span>' + when(x.ts) + '</span><b>' + f1(x.soc_start, 0) + '→' + f1(x.soc_end, 0) +
        ' · ' + f1(x.km || 0, 0) + ' км</b></div>').join('') : '<div class="hint">Нет данных</div>') +
      card('Климат', null, kv([
        ['Сеансов', esc((st.climate || {}).count || 0)],
        ['Энергия', f1((st.climate || {}).kwh || 0) + ' кВт·ч'],
        ['Время', f1((st.climate || {}).hours || 0) + ' ч']
      ]));
  }

  /* ---------- выбор машины ---------- */
  function deviceModal() {
    const devs = DATA.devices.devices || [];
    const old = $('#devModal');
    if (old) old.remove();
    const box = document.createElement('div');
    box.id = 'devModal';
    box.style.cssText = 'position:fixed;inset:0;z-index:100;background:rgba(0,0,0,.55);display:grid;place-items:center;padding:20px';
    box.innerHTML = '<div style="width:min(420px,100%);background:var(--panel);border:1px solid var(--line);border-radius:18px;padding:18px">' +
      '<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:12px"><b>Выбрать машину</b><button id="devClose" style="border:0;background:none;color:var(--muted);font-size:20px">×</button></div>' +
      (devs.length ? devs.map((d) => '<button data-dev="' + esc(d.id) + '" style="display:block;width:100%;text-align:left;padding:12px;margin-bottom:8px;border-radius:12px;border:1px solid var(--line);background:var(--glass)">' +
        esc(d.name || d.login) + (d.online ? ' · онлайн' : '') + '</button>').join('') : '<div class="hint">Нет устройств</div>') + '</div>';
    document.body.appendChild(box);
    $('#devClose').onclick = () => box.remove();
    box.addEventListener('click', (e) => { if (e.target === box) box.remove(); });
    $$('[data-dev]', box).forEach((b) => {
      b.onclick = async () => {
        try {
          await api('/api/device/select', { device: Number(b.dataset.dev) });
          toast('Машина выбрана');
          setTimeout(() => location.reload(), 600);
        } catch (e) { toast(e.message); }
      };
    });
  }

  /* ---------- опрос ---------- */
  let pollTimer = null, failStreak = 0, pollBusy = false;
  async function pollNow() {
    if (pollBusy) return;
    pollBusy = true;
    try {
      const j = await api('/api/status');
      DATA.status = j;
      applyStatus(j);
      failStreak = 0;
    } catch (e) {
      if (e.status === 401) { location.href = '/login'; return; }
      failStreak++;
      $('#connText').textContent = 'нет связи с сервером';
    } finally {
      pollBusy = false;
      const delay = Math.min(30000, 3000 * Math.pow(2, Math.min(failStreak, 4)));
      clearTimeout(pollTimer);
      pollTimer = setTimeout(pollNow, delay);
    }
  }

  /* ---------- старт ---------- */
  function init() {
    try { setTheme(localStorage.getItem('radius-theme') || 'dark'); } catch (e) { setTheme('dark'); }
    $('#btnTheme').addEventListener('click', () =>
      setTheme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark'));
    $('#sTheme').addEventListener('click', () => {
      setTheme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark');
      toast('Тема переключена');
    });
    $('#btnRefresh').addEventListener('click', () => { pollNow(); loadPeriod(period); toast('Данные обновлены'); });
    $('#btnDevice').addEventListener('click', deviceModal);
    $('#core').addEventListener('click', () =>
      $('#dock').classList.contains('open') ? closeDock() : openDock());
    $('#veil').addEventListener('click', closeDock);
    $('#btnSheet').addEventListener('click', () =>
      sheet.classList.contains('open') ? closeSheet() : openSheet());
    $('#sClose').addEventListener('click', closeSheet);
    $$('.stab').forEach((b) => b.addEventListener('click', () => selectPane(b.dataset.pane)));
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') { closeSheet(); closeDock(); const m = $('#devModal'); if (m) m.remove(); }
    });
    bindHold('.orbit-btn', (b) => { run(b.dataset.act); closeDock(); });
    $$('.hs').forEach((h) => h.addEventListener('click', () => run(h.dataset.act)));

    $('#tripsPeriods').innerHTML = PERIODS.map((p) =>
      '<button data-days="' + p.d + '"' + (p.d === period ? ' class="on"' : '') + '>' + p.label + '</button>').join('');
    $$('#tripsPeriods button').forEach((b) => b.onclick = () => {
      $$('#tripsPeriods button').forEach((x) => x.classList.remove('on'));
      b.classList.add('on');
      loadPeriod(Number(b.dataset.days));
    });
    $('#statPeriods').innerHTML = PERIODS.map((p) =>
      '<button data-days="' + p.d + '"' + (p.d === period ? ' class="on"' : '') + '>' + p.label + '</button>').join('');
    $$('#statPeriods button').forEach((b) => b.onclick = () => {
      $$('#statPeriods button').forEach((x) => x.classList.remove('on'));
      b.classList.add('on');
      loadPeriod(Number(b.dataset.days));
    });
    $$('#statSubTabs button').forEach((b) => b.onclick = () => {
      statSub = b.dataset.sub;
      $$('#statSubTabs button').forEach((x) => x.classList.toggle('on', x === b));
      renderStats();
    });

    function setTemp(v) {
      S.setpoint = Math.max(16, Math.min(30, Math.round(v * 2) / 2));
      sync();
    }
    $('#clUp').onclick = () => setTemp(S.setpoint + 0.5);
    $('#clDown').onclick = () => setTemp(S.setpoint - 0.5);
    $('#clBtn').onclick = () => run('climate');
    $$('#clChips .chip').forEach((c) => c.onclick = () => {
      $$('#clChips .chip').forEach((x) => x.classList.remove('on'));
      c.classList.add('on');
      toast('Вентилятор: ' + c.textContent);
    });
    $('#clPlan').onclick = async () => {
      const parts = ($('#clAt').value || '08:00').split(':');
      const mins = Math.max(5, Math.min(60, Number($('#clMinutes').value) || 15));
      const target = new Date();
      target.setHours(Number(parts[0]) || 8, Number(parts[1]) || 0, 0, 0);
      if (target <= new Date()) target.setDate(target.getDate() + 1);
      const start = new Date(target.getTime() - mins * 60000);
      if (start <= new Date()) { target.setDate(target.getDate() + 1); start.setDate(start.getDate() + 1); }
      try {
        await api('/api/sched', { at: Math.floor(start.getTime() / 1000), minutes: mins });
        toast('Прогрев к ' + clock(target.getTime() / 1000));
        loadMeta();
      } catch (e) { toast(e.message); }
    };
    $('#clPlanRefresh').onclick = () => loadMeta();

    $('#cfgSave').onclick = async () => {
      const hint = $('#cfgHint');
      hint.textContent = 'Отправляю…';
      let ok = 0;
      for (const el of $$('#cfgFields [data-cfg]')) {
        let value;
        if (el.type === 'checkbox') value = el.checked ? 1 : 0;
        else if (el.value === '') continue;
        else value = el.type === 'number' ? Number(el.value) : el.value;
        try { await api('/api/config', { key: el.dataset.cfg, value }); ok++; } catch (e) { /* тихо */ }
      }
      hint.textContent = 'Отправлено параметров: ' + ok + '; плата применит их в течение 10 с';
      setTimeout(loadMeta, 5000);
    };

    fitDock();
    window.addEventListener('resize', fitDock);
    pollNow();
    loadMeta();
    loadPeriod(period);
    loadFirmwareList();
    setInterval(() => { loadPeriod(period); loadMeta(); }, 60000);
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
