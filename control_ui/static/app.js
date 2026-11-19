'use strict';

/* ============================================================ helpers */

const $ = (selector, root = document) => root.querySelector(selector);
const view = $('#view');
let state = null;            // latest /api/state snapshot
let presets = [];
let session = null;
let events = null;           // EventSource
let stateListeners = [];     // per-view update callbacks

function escapeHtml(text) {
  return String(text).replace(/[&<>"']/g, ch => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[ch]));
}

function fmtAz(value) {
  return value === null || value === undefined ? '—' : value.toFixed(1) + '°';
}

function norm(deg) {
  return ((deg % 360) + 360) % 360;
}

let toastTimer = 0;
function toast(message, isError = false) {
  const box = $('#toast');
  box.textContent = message;
  box.className = 'toast show' + (isError ? ' error' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { box.className = 'toast'; }, isError ? 4000 : 2000);
}

async function api(method, path, body) {
  const options = {method, headers: {}, cache: 'no-store'};
  if (body !== undefined) {
    options.headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(body);
  }
  const response = await fetch(path, options);
  let data = null;
  try { data = await response.json(); } catch (error) { data = null; }
  if (response.status === 401 && path !== '/api/login') {
    session = null;
    stopEvents();
    location.hash = '#/login';
    throw new Error('Потрібно увійти');
  }
  if (!response.ok) throw new Error((data && data.error) || ('HTTP ' + response.status));
  return data;
}

/* Run an API action and report errors as a toast. */
async function act(promise, okMessage) {
  try {
    const result = await promise;
    if (okMessage) toast(okMessage);
    return result;
  } catch (error) {
    toast(error.message, true);
    return null;
  }
}

/* Is the azimuth inside the reachable arc? null when not calibrated. */
function reachable(az, st = state) {
  if (!st || !st.sector) return null;
  const offset = norm(az - st.sector.start);
  return offset <= st.sector.span + 1e-6 || offset >= 360 - 1e-6;
}

/* ============================================================ live state */

function startEvents() {
  if (events) return;
  events = new EventSource('/api/events');
  events.onmessage = message => {
    state = JSON.parse(message.data);
    updateBadge();
    stateListeners.forEach(fn => fn(state));
  };
  events.onerror = () => {
    if (events && events.readyState === EventSource.CLOSED) {
      stopEvents();
      // Closed for good: either the session ended or the server restarted.
      api('GET', '/api/session').then(() => setTimeout(startEvents, 2000)).catch(() => {});
    }
  };
}

function stopEvents() {
  if (events) events.close();
  events = null;
}

function updateBadge() {
  const badge = $('#link-badge');
  if (!state) { badge.textContent = '—'; badge.className = 'link-badge'; return; }
  badge.textContent = state.online ? 'Плата на зв’язку' : 'Немає зв’язку з платою';
  badge.className = 'link-badge ' + (state.online ? 'online' : 'offline');
  badge.title = state.online ? '' : (state.error || '');
}

async function loadPresets() {
  presets = (await api('GET', '/api/presets')) || [];
  return presets;
}

/* ============================================================ compass */

const C = 200;          // center
const R = 160;          // ring radius

function point(radius, az) {
  const rad = az * Math.PI / 180;
  return [C + radius * Math.sin(rad), C - radius * Math.cos(rad)];
}

function arcPath(radius, start, span) {
  if (span >= 359.999) {
    const [x1, y1] = point(radius, start);
    const [x2, y2] = point(radius, start + 180);
    return `M${x1} ${y1}A${radius} ${radius} 0 1 1 ${x2} ${y2}A${radius} ${radius} 0 1 1 ${x1} ${y1}`;
  }
  const [x1, y1] = point(radius, start);
  const [x2, y2] = point(radius, start + span);
  return `M${x1} ${y1}A${radius} ${radius} 0 ${span > 180 ? 1 : 0} 1 ${x2} ${y2}`;
}

function compassSvg() {
  let ticks = '';
  for (let az = 0; az < 360; az += 5) {
    const major = az % 30 === 0;
    const [x1, y1] = point(R - 12, az);
    const [x2, y2] = point(R - (major ? 26 : (az % 10 === 0 ? 20 : 16)), az);
    ticks += `<line class="tick${major ? ' major' : ''}" x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}"/>`;
    if (major && az % 90 !== 0) {
      const [lx, ly] = point(R - 40, az);
      ticks += `<text class="tick-label" x="${lx}" y="${ly}">${az}</text>`;
    }
  }
  const cardinals = [['N', 0], ['E', 90], ['S', 180], ['W', 270]].map(([label, az]) => {
    const [x, y] = point(R - 42, az);
    return `<text class="cardinal" x="${x}" y="${y}">${label}</text>`;
  }).join('');

  return `
  <svg class="compass" id="compass" viewBox="0 0 400 400" role="img" aria-label="Компас: поточний азимут">
    <circle class="ring-bg" cx="${C}" cy="${C}" r="${R + 14}"/>
    <path class="forbidden" id="arc-forbidden" d=""/>
    <path class="allowed" id="arc-allowed" d=""/>
    <line class="limit" id="limit-a" x1="0" y1="0" x2="0" y2="0" visibility="hidden"/>
    <line class="limit" id="limit-b" x1="0" y1="0" x2="0" y2="0" visibility="hidden"/>
    ${ticks}${cardinals}
    <line class="pick" id="pick-line" x1="${C}" y1="${C}" x2="${C}" y2="${C}" visibility="hidden"/>
    <polygon class="target" id="target-mark" points="0,0" visibility="hidden"/>
    <g id="needle" visibility="hidden">
      <line class="needle" x1="${C}" y1="${C}" x2="${C}" y2="${C - R + 34}"/>
      <polygon class="needle-head" points="${C},${C - R + 22} ${C - 9},${C - R + 42} ${C + 9},${C - R + 42}"/>
    </g>
    <circle class="hub" cx="${C}" cy="${C}" r="6"/>
    <rect class="readout-plate" x="${C - 74}" y="${C + 22}" width="148" height="66" rx="10"/>
    <text class="readout" id="readout" x="${C}" y="${C + 56}">—</text>
    <text class="readout-sub" id="readout-sub" x="${C}" y="${C + 78}"></text>
  </svg>`;
}

function setLine(el, az, r1, r2) {
  const [x1, y1] = point(r1, az);
  const [x2, y2] = point(r2, az);
  el.setAttribute('x1', x1); el.setAttribute('y1', y1);
  el.setAttribute('x2', x2); el.setAttribute('y2', y2);
  el.setAttribute('visibility', 'visible');
}

function updateCompass(st) {
  const svg = $('#compass');
  if (!svg) return;
  const allowed = $('#arc-allowed', svg);
  const forbidden = $('#arc-forbidden', svg);
  if (st.sector) {
    const {start, span} = st.sector;
    allowed.setAttribute('d', arcPath(R + 7, start, span));
    forbidden.setAttribute('d', span < 359.999 ? arcPath(R + 7, start + span, 360 - span) : '');
    setLine($('#limit-a', svg), start, R - 8, R + 18);
    setLine($('#limit-b', svg), start + span, R - 8, R + 18);
  } else {
    allowed.setAttribute('d', '');
    forbidden.setAttribute('d', '');
  }

  const needle = $('#needle', svg);
  if (st.azimuth !== null) {
    needle.setAttribute('transform', `rotate(${st.azimuth} ${C} ${C})`);
    needle.setAttribute('visibility', 'visible');
  } else {
    needle.setAttribute('visibility', 'hidden');
  }

  const target = $('#target-mark', svg);
  if (st.target_azimuth !== null && st.moving) {
    const tip = point(R + 2, st.target_azimuth);
    const left = point(R + 22, st.target_azimuth - 4);
    const right = point(R + 22, st.target_azimuth + 4);
    target.setAttribute('points', `${tip} ${left} ${right}`);
    target.setAttribute('visibility', 'visible');
  } else {
    target.setAttribute('visibility', 'hidden');
  }

  $('#readout', svg).textContent = st.calibrated ? fmtAz(st.azimuth) : (st.pulse ? st.pulse + ' µs' : '—');
  $('#readout-sub', svg).textContent = !st.calibrated ? 'не прив’язано до азимута'
    : (st.moving ? 'рух до ' + fmtAz(st.target_azimuth) : 'стоїть');
}

function showPick(az) {
  const line = $('#pick-line');
  if (!line) return;
  if (az === null) { line.setAttribute('visibility', 'hidden'); return; }
  setLine(line, az, 8, R - 2);
}

/* ============================================================ views */

function viewLogin() {
  $('#topbar').hidden = true;
  $('#banner').hidden = true;
  view.innerHTML = `
  <div class="login-wrap">
    <form class="card login-card" id="login-form">
      <h1>Вхід</h1>
      <label for="login-user">Користувач</label>
      <input id="login-user" type="text" autocomplete="username" required>
      <label for="login-pass">Пароль</label>
      <input id="login-pass" type="password" autocomplete="current-password" required>
      <button class="btn btn-primary" type="submit" style="width:100%">Увійти</button>
      <p class="hint bad" id="login-error"></p>
    </form>
  </div>`;
  $('#login-user').focus();
  $('#login-form').addEventListener('submit', async event => {
    event.preventDefault();
    $('#login-error').textContent = '';
    try {
      await api('POST', '/api/login', {username: $('#login-user').value, password: $('#login-pass').value});
      await enterApp();
      location.hash = '#/control';
    } catch (error) {
      $('#login-error').textContent = error.message;
    }
  });
}

function presetItems() {
  if (!presets.length) return '<p class="muted small">Пресетів ще немає.</p>';
  return presets.map(p => {
    const ok = reachable(p.azimuth);
    return `<div class="preset-item${ok === false ? ' unreachable' : ''}">
      <span class="name" title="${escapeHtml(p.name)}">${escapeHtml(p.name)}</span>
      <span class="num muted">${p.azimuth.toFixed(1)}°</span>
      <button class="btn btn-sm btn-primary" type="button" data-go="${p.azimuth}" ${ok === false ? 'disabled title="В недосяжній зоні"' : ''}>Їхати</button>
    </div>`;
  }).join('');
}

function viewControl() {
  view.innerHTML = `
  <div class="control-layout">
    <section class="card compass-card">
      ${compassSvg()}
      <div class="legend">
        <span><i style="background:var(--ok)"></i>Допустимий сектор</span>
        <span><i style="background:var(--danger)"></i>Недосяжна зона</span>
        <span><i style="background:var(--needle)"></i>Поточний азимут</span>
        <span><i style="background:var(--target)"></i>Ціль</span>
      </div>
      <p class="muted small">Клік по колу підставляє азимут у поле «Азимут цілі».</p>
    </section>

    <div class="stack">
      <section class="card">
        <div class="readouts">
          <div><label>Поточний азимут</label><div class="big" id="ro-az">—</div></div>
          <div><label>Ціль</label><div class="big" id="ro-target">—</div></div>
          <div><label>Імпульс</label><div class="num" id="ro-pulse">—</div></div>
          <div><label>Стан</label><span class="state-pill" id="ro-state">—</span></div>
        </div>
      </section>

      <section class="card">
        <h2>Поворот на азимут</h2>
        <form class="row" id="goto-form">
          <input class="grow num" id="goto-az" type="number" min="0" max="359.9" step="0.1" placeholder="Азимут цілі, °" aria-label="Азимут цілі" required>
          <button class="btn btn-primary" type="submit" id="goto-btn">Повернути</button>
          <button class="btn btn-danger" type="button" id="stop-btn">Стоп</button>
        </form>
        <div class="hint" id="goto-hint"></div>
        <label style="margin-top:10px">Підвести вручну</label>
        <div class="jog">
          <button class="btn" type="button" data-jog="-10">−10°</button>
          <button class="btn" type="button" data-jog="-1">−1°</button>
          <button class="btn" type="button" data-jog="1">+1°</button>
          <button class="btn" type="button" data-jog="10">+10°</button>
        </div>
      </section>

      <section class="card">
        <h2>Швидкість</h2>
        <div class="row">
          <input class="grow" id="speed-range" type="range" min="0" max="255" aria-label="Швидкість">
          <span class="num" id="speed-value" style="min-width:3ch;text-align:right">—</span>
        </div>
        <p class="muted small">0 — тримати позицію, 1–254 — плавно (254 ≈ максимум серво), 255 — миттєво.</p>
      </section>

      <section class="card">
        <h2>Прив’язати до азимута</h2>
        <p class="muted small">Наведіть механізм на відомий орієнтир, дочекайтеся зупинки і введіть його справжній азимут.</p>
        <form class="row" id="bind-form">
          <input class="grow num" id="bind-az" type="number" min="0" max="359.9" step="0.1" placeholder="Справжній азимут, °" aria-label="Справжній азимут" required>
          <button class="btn" type="submit">Прив’язати</button>
        </form>
        <div class="hint" id="bind-hint"></div>
      </section>

      <section class="card">
        <div class="row" style="justify-content:space-between;margin-bottom:8px">
          <h2 style="margin:0">Пресети</h2>
          <a class="small" href="#/presets">Керувати</a>
        </div>
        <div class="preset-list" id="preset-list"></div>
        <form class="row" id="quick-preset" style="margin-top:10px">
          <input class="grow" id="quick-name" type="text" maxlength="40" placeholder="Назва" aria-label="Назва пресету" required>
          <button class="btn" type="submit">Зберегти поточний</button>
        </form>
      </section>
    </div>
  </div>`;

  const gotoInput = $('#goto-az');
  const speedRange = $('#speed-range');
  let speedEditing = false;

  function renderPresets() {
    $('#preset-list').innerHTML = presetItems();
  }

  function updateGotoHint() {
    const hint = $('#goto-hint');
    const raw = gotoInput.value;
    if (!state || !state.calibrated) {
      hint.textContent = 'Спершу прив’яжіть поточне положення до азимута.';
      hint.className = 'hint bad';
      $('#goto-btn').disabled = true;
      showPick(null);
      return;
    }
    $('#goto-btn').disabled = false;
    if (raw === '') { hint.textContent = ''; hint.className = 'hint'; showPick(null); return; }
    const az = norm(parseFloat(raw));
    showPick(az);
    if (reachable(az)) {
      hint.textContent = 'Досяжно';
      hint.className = 'hint good';
    } else {
      hint.textContent = 'Азимут у недосяжній зоні';
      hint.className = 'hint bad';
    }
  }

  function onState(st) {
    updateCompass(st);
    $('#ro-az').textContent = st.calibrated ? fmtAz(st.azimuth) : '—';
    $('#ro-target').textContent = st.calibrated ? fmtAz(st.target_azimuth) : '—';
    $('#ro-pulse').textContent = st.pulse ? `${st.pulse} µs (ціль ${st.target_pulse} µs)` : 'вихід вимкнено';
    const pill = $('#ro-state');
    pill.textContent = !st.online ? 'немає зв’язку' : (st.moving ? 'рухається' : 'стоїть');
    pill.className = 'state-pill' + (st.moving ? ' moving' : '');
    if (!speedEditing && st.speed !== null && st.speed !== undefined) {
      speedRange.value = st.speed;
      $('#speed-value').textContent = st.speed;
    }
    const bindHint = $('#bind-hint');
    if (!bindHint.dataset.locked) {
      bindHint.textContent = st.calibrated ? '' : 'Ще не прив’язано: азимут невідомий.';
      bindHint.className = 'hint' + (st.calibrated ? '' : ' bad');
    }
    updateGotoHint();
  }

  stateListeners = [onState];
  if (state) onState(state);
  loadPresets().then(renderPresets).catch(() => {});

  // compass click -> fill target input
  $('#compass').addEventListener('click', event => {
    const svg = event.currentTarget;
    const box = svg.getBoundingClientRect();
    const x = (event.clientX - box.left) / box.width * 400 - C;
    const y = (event.clientY - box.top) / box.height * 400 - C;
    const az = norm(Math.atan2(x, -y) * 180 / Math.PI);
    gotoInput.value = az.toFixed(1);
    updateGotoHint();
  });

  gotoInput.addEventListener('input', updateGotoHint);
  $('#goto-form').addEventListener('submit', async event => {
    event.preventDefault();
    const az = norm(parseFloat(gotoInput.value));
    await act(api('POST', '/api/goto', {azimuth: az}), 'Поворот на ' + az.toFixed(1) + '°');
  });
  $('#stop-btn').addEventListener('click', () => act(api('POST', '/api/stop', {}), 'Зупинено'));
  view.querySelectorAll('[data-jog]').forEach(button => {
    button.addEventListener('click', () => act(api('POST', '/api/jog', {degrees: parseFloat(button.dataset.jog)})));
  });

  speedRange.addEventListener('input', () => {
    speedEditing = true;
    $('#speed-value').textContent = speedRange.value;
  });
  speedRange.addEventListener('change', async () => {
    await act(api('POST', '/api/speed', {speed: parseInt(speedRange.value, 10)}), 'Швидкість ' + speedRange.value);
    speedEditing = false;
  });

  $('#bind-form').addEventListener('submit', async event => {
    event.preventDefault();
    const az = norm(parseFloat($('#bind-az').value));
    const bindHint = $('#bind-hint');
    try {
      const cal = await api('POST', '/api/calibrate', {azimuth: az});
      bindHint.dataset.locked = '1';
      bindHint.textContent = `Прив’язано: ${cal.pulse} µs = ${cal.azimuth.toFixed(1)}°`;
      bindHint.className = 'hint good';
      setTimeout(() => { delete bindHint.dataset.locked; }, 4000);
      $('#bind-az').value = '';
      renderPresets();
    } catch (error) {
      bindHint.textContent = error.message;
      bindHint.className = 'hint bad';
    }
  });

  $('#preset-list').addEventListener('click', event => {
    const button = event.target.closest('[data-go]');
    if (!button) return;
    const az = parseFloat(button.dataset.go);
    act(api('POST', '/api/goto', {azimuth: az}), 'Поворот на ' + az.toFixed(1) + '°');
  });
  $('#quick-preset').addEventListener('submit', async event => {
    event.preventDefault();
    const created = await act(api('POST', '/api/presets', {name: $('#quick-name').value}), 'Пресет збережено');
    if (created) {
      $('#quick-name').value = '';
      await loadPresets();
      renderPresets();
    }
  });
}

function viewPresets() {
  view.innerHTML = `
  <h1>Пресети</h1>
  <section class="card" style="margin-bottom:16px">
    <h2>Новий пресет</h2>
    <form class="row" id="preset-form">
      <input class="grow" id="preset-name" type="text" maxlength="40" placeholder="Назва" aria-label="Назва" required>
      <input class="num" id="preset-az" type="number" min="0" max="359.9" step="0.1" placeholder="Азимут (пусто = поточний)" aria-label="Азимут" style="width:220px">
      <button class="btn btn-primary" type="submit">Додати</button>
    </form>
  </section>
  <section class="card">
    <table class="table">
      <thead><tr><th>Назва</th><th>Азимут</th><th>Досяжність</th><th></th></tr></thead>
      <tbody id="preset-rows"></tbody>
    </table>
  </section>`;

  function render() {
    const rows = $('#preset-rows');
    if (!presets.length) {
      rows.innerHTML = '<tr><td colspan="4" class="muted">Пресетів ще немає.</td></tr>';
      return;
    }
    rows.innerHTML = presets.map(p => {
      const ok = reachable(p.azimuth);
      const status = ok === null ? '<span class="muted">не прив’язано</span>'
        : (ok ? '<span class="hint good">так</span>' : '<span class="hint bad">недосяжна зона</span>');
      return `<tr>
        <td>${escapeHtml(p.name)}</td>
        <td class="num">${p.azimuth.toFixed(1)}°</td>
        <td>${status}</td>
        <td class="row" style="justify-content:flex-end">
          <button class="btn btn-sm btn-primary" type="button" data-go="${p.id}" ${ok ? '' : 'disabled'}>Їхати</button>
          <button class="btn btn-sm" type="button" data-rename="${p.id}">Перейменувати</button>
          <button class="btn btn-sm" type="button" data-delete="${p.id}">Видалити</button>
        </td>
      </tr>`;
    }).join('');
  }

  stateListeners = [];
  loadPresets().then(render).catch(() => {});
  if (!state) setTimeout(render, 600);

  $('#preset-form').addEventListener('submit', async event => {
    event.preventDefault();
    const raw = $('#preset-az').value;
    const body = {name: $('#preset-name').value, azimuth: raw === '' ? null : norm(parseFloat(raw))};
    if (await act(api('POST', '/api/presets', body), 'Пресет збережено')) {
      $('#preset-name').value = '';
      $('#preset-az').value = '';
      await loadPresets();
      render();
    }
  });

  $('#preset-rows').addEventListener('click', async event => {
    const go = event.target.closest('[data-go]');
    const rename = event.target.closest('[data-rename]');
    const remove = event.target.closest('[data-delete]');
    if (go) {
      const preset = presets.find(p => p.id === go.dataset.go);
      await act(api('POST', '/api/goto', {azimuth: preset.azimuth}), 'Поворот на ' + preset.azimuth.toFixed(1) + '°');
    } else if (rename) {
      const preset = presets.find(p => p.id === rename.dataset.rename);
      const name = prompt('Нова назва', preset.name);
      if (name && await act(api('PUT', '/api/presets/' + preset.id, {name}), 'Перейменовано')) {
        await loadPresets();
        render();
      }
    } else if (remove) {
      const preset = presets.find(p => p.id === remove.dataset.delete);
      if (confirm(`Видалити пресет «${preset.name}»?`) && await act(api('DELETE', '/api/presets/' + preset.id), 'Видалено')) {
        await loadPresets();
        render();
      }
    }
  });
}

async function viewSettings() {
  view.innerHTML = '<h1>Налаштування</h1><p class="muted">Завантаження…</p>';
  stateListeners = [];
  let config;
  try { config = await api('GET', '/api/config'); } catch (error) { return; }
  const m = config.mapping;
  const cal = config.calibration;

  view.innerHTML = `
  <h1>Налаштування</h1>
  <div class="stack">
    <form class="card" id="board-form">
      <h2>Плата</h2>
      <div class="field-grid">
        <div><label for="b-host">IP плати</label><input id="b-host" type="text" value="${escapeHtml(config.board.host)}" required></div>
        <div><label for="b-port">Порт JSON API (UDP)</label><input id="b-port" type="number" min="1" max="65535" value="${config.board.port}" required></div>
        <div><label for="b-timeout">Таймаут, с</label><input id="b-timeout" type="number" min="0.05" max="5" step="0.05" value="${config.board.timeout_s}" required></div>
        <div><label for="b-servo">Серво азимута</label>
          <select id="b-servo"><option value="1">Servo 1</option><option value="2">Servo 2</option></select></div>
      </div>
      <p class="muted small">JSON API на платі має бути увімкнений у режимі UDP (сторінка Network плати).</p>
      <button class="btn btn-primary" type="submit">Зберегти</button>
    </form>

    <form class="card" id="mapping-form">
      <h2>Механіка</h2>
      <div class="field-grid">
        <div><label for="m-min">Імпульс min, µs</label><input id="m-min" type="number" min="1" max="3000" value="${m.pulse_min}" required></div>
        <div><label for="m-max">Імпульс max, µs</label><input id="m-max" type="number" min="1" max="3000" value="${m.pulse_max}" required></div>
        <div><label for="m-range">Діапазон повороту, °</label><input id="m-range" type="number" min="1" max="360" step="0.1" value="${m.range_deg}" required></div>
        <div><label for="m-invert">Більший імпульс крутить</label>
          <select id="m-invert"><option value="0">За годинниковою</option><option value="1">Проти годинникової</option></select></div>
      </div>
      <p class="muted small" id="m-scale"></p>
      <p class="small">${cal ? `Прив’язка: <span class="num">${cal.pulse} µs = ${cal.azimuth.toFixed(1)}°</span>` : '<span class="hint bad">Не прив’язано до азимута.</span>'}</p>
      <div class="row">
        <button class="btn btn-primary" type="submit">Зберегти</button>
        <button class="btn" type="button" id="clear-cal" ${cal ? '' : 'disabled'}>Скинути прив’язку</button>
      </div>
    </form>

    <form class="card" id="password-form">
      <h2>Вхід</h2>
      <div class="field-grid">
        <div><label for="p-user">Користувач</label><input id="p-user" type="text" value="${escapeHtml(config.username)}" autocomplete="username" required></div>
        <div><label for="p-current">Поточний пароль</label><input id="p-current" type="password" autocomplete="current-password" required></div>
        <div><label for="p-new">Новий пароль (від 6 символів)</label><input id="p-new" type="password" minlength="6" autocomplete="new-password" required></div>
      </div>
      <p class="muted small">Після зміни всі сесії завершаться, потрібно буде увійти знову.</p>
      <button class="btn btn-primary" type="submit">Змінити</button>
    </form>
  </div>`;

  $('#b-servo').value = String(config.servo);
  $('#m-invert').value = m.invert ? '1' : '0';

  function updateScale() {
    const min = parseFloat($('#m-min').value), max = parseFloat($('#m-max').value), range = parseFloat($('#m-range').value);
    $('#m-scale').textContent = max > min && range > 0
      ? `Масштаб: ${(range / (max - min)).toFixed(4)} °/µs, крок 1 µs ≈ ${(range / (max - min)).toFixed(2)}°. Недосяжна зона: ${(360 - range).toFixed(1)}°.`
      : 'Перевірте значення.';
  }
  ['m-min', 'm-max', 'm-range'].forEach(id => $('#' + id).addEventListener('input', updateScale));
  updateScale();

  $('#board-form').addEventListener('submit', async event => {
    event.preventDefault();
    const servo = parseInt($('#b-servo').value, 10);
    if (servo !== config.servo && config.calibration && !confirm('Зміна серво скине прив’язку до азимута. Продовжити?')) return;
    const result = await act(api('PUT', '/api/config', {
      board: {host: $('#b-host').value, port: parseInt($('#b-port').value, 10), timeout_s: parseFloat($('#b-timeout').value)},
      servo,
    }), 'Збережено');
    if (result) viewSettings();
  });

  $('#mapping-form').addEventListener('submit', async event => {
    event.preventDefault();
    const result = await act(api('PUT', '/api/config', {mapping: {
      pulse_min: parseInt($('#m-min').value, 10),
      pulse_max: parseInt($('#m-max').value, 10),
      range_deg: parseFloat($('#m-range').value),
      invert: $('#m-invert').value === '1',
    }}), 'Збережено');
    if (result) viewSettings();
  });

  $('#clear-cal').addEventListener('click', async () => {
    if (!confirm('Скинути прив’язку до азимута?')) return;
    if (await act(api('PUT', '/api/config', {clear_calibration: true}), 'Прив’язку скинуто')) viewSettings();
  });

  $('#password-form').addEventListener('submit', async event => {
    event.preventDefault();
    const ok = await act(api('POST', '/api/password', {
      username: $('#p-user').value, current: $('#p-current').value, new: $('#p-new').value,
    }), 'Пароль змінено');
    if (ok) {
      session = null;
      stopEvents();
      location.hash = '#/login';
    }
  });
}

/* ============================================================ router */

const routes = {'/control': viewControl, '/presets': viewPresets, '/settings': viewSettings};

async function enterApp() {
  session = await api('GET', '/api/session');
  $('#topbar').hidden = false;
  const banner = $('#banner');
  banner.hidden = !session.default_password;
  banner.textContent = 'Використовується пароль за замовчуванням (admin). Змініть його в Налаштуваннях.';
  startEvents();
}

async function route() {
  const path = location.hash.replace(/^#/, '') || '/control';
  if (path === '/login') {
    stopEvents();
    stateListeners = [];
    return viewLogin();
  }
  if (!session) {
    try {
      await enterApp();
    } catch (error) {
      location.hash = '#/login';
      return;
    }
  }
  const render = routes[path];
  if (!render) { location.hash = '#/control'; return; }
  document.querySelectorAll('.nav a').forEach(a => a.classList.toggle('active', a.dataset.route === path));
  render();
}

$('#logout').addEventListener('click', async () => {
  await api('POST', '/api/logout', {}).catch(() => {});
  session = null;
  stopEvents();
  location.hash = '#/login';
});

window.addEventListener('hashchange', route);
route();
