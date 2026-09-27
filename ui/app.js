'use strict';
/* Wallpaper Plus settings UI.
   Talks to the native host (src/settings/bridge.cpp) with rpc(cmd, args). Every change is saved
   immediately: settings go to wallpaper.ini (the player applies them live), the library to
   library.json. Nothing here animates or plays while idle; videos play only while hovered. */

const $ = (sel, root = document) => root.querySelector(sel);
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

// ---------- bridge ----------

const pending = new Map();
const listeners = {};
let nextId = 1;
const hasHost = !!(window.chrome && window.chrome.webview);

if (hasHost) {
  window.chrome.webview.addEventListener('message', (e) => {
    const m = e.data;
    if (m.event) return (listeners[m.event] || []).forEach((f) => f(m.data));
    const p = pending.get(m.id);
    if (!p) return;
    pending.delete(m.id);
    m.error ? p.reject(new Error(m.error)) : p.resolve(m.result);
  });
}
function rpc(cmd, args = {}) {
  if (!hasHost) return Promise.reject(new Error('not running inside Wallpaper Plus'));
  return new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    window.chrome.webview.postMessage({ id, cmd, args });
  });
}
const onHost = (event, f) => (listeners[event] ||= []).push(f);

// ---------- window frame ----------
// The top bar is the window's title bar (the host removed the Windows one).

function applyWindowState(w) {
  document.body.toggleAttribute('data-maximized', !!w.maximized);
  document.body.toggleAttribute('data-inactive', !w.active);
  const max = $('[data-win="maximize"]');
  const label = w.maximized ? 'Restore' : 'Maximize';
  max.title = label;
  max.setAttribute('aria-label', label);
}
onHost('window', applyWindowState);
document.addEventListener('pointerdown', (e) => {
  const edge = e.button === 0 && e.target.closest('[data-edge]');
  if (!edge) return;
  e.preventDefault();
  rpc('window', { action: 'resize', edge: edge.dataset.edge });
});

// ---------- state ----------

const state = {
  monitors: [],
  config: null,
  library: { version: 1, folders: [], items: [] },
  selected: 1,
  view: 'monitors',
  playerRunning: true,
  pick: null,       // { monitor, mode: 'replace' | 'add' } while choosing from the library
  search: '',
  sort: 'added',
};
const mediaUrls = new Map(); // path -> streaming URL served by the host

function hashId(text) {
  // Two FNV-1a passes with different seeds -> 16 hex chars; stable per path.
  let a = 0x811c9dc5, b = 0x01000193 ^ 0x9747b28c;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    a = Math.imul(a ^ c, 0x01000193) >>> 0;
    b = Math.imul(b ^ c, 0x01000193 + 2) >>> 0;
  }
  return a.toString(16).padStart(8, '0') + b.toString(16).padStart(8, '0');
}
const keyOf = (path) => path.toLowerCase();
const baseName = (path) => path.split(/[\\/]/).pop().replace(/\.[^.]+$/, '');
const itemByPath = (path) => state.library.items.find((i) => keyOf(i.path) === keyOf(path));
const monitorByNumber = (n) => state.monitors.find((m) => m.number === n);

function fmtDuration(sec) {
  if (!sec || !isFinite(sec)) return '';
  sec = Math.round(sec);
  const m = Math.floor(sec / 60), s = sec % 60;
  return m >= 60 ? `${Math.floor(m / 60)}:${String(m % 60).padStart(2, '0')}:${String(s).padStart(2, '0')}` : `${m}:${String(s).padStart(2, '0')}`;
}
function itemMeta(item) {
  if (!item) return '';
  const parts = [];
  if (item.width) parts.push(`${item.width}×${item.height}`);
  if (item.duration) parts.push(fmtDuration(item.duration));
  return parts.join(' · ');
}

function defaultSetting() {
  return { videos: [], fit: 'fill', brightness: 100, speed: 1, rotateMinutes: 30, shuffle: false };
}
function settingFor(n) {
  const c = state.config;
  c.monitors ||= {};
  if (!c.monitors[n]) c.monitors[n] = structuredClone(c.defaults || defaultSetting());
  return c.monitors[n];
}

// ---------- saving ----------

let configTimer = 0, libraryTimer = 0;
function saveConfig() {
  clearTimeout(configTimer);
  configTimer = setTimeout(() => {
    // New monitors that get plugged in later start with monitor 1's wallpaper.
    const first = state.monitors[0];
    if (first) state.config.defaults = structuredClone(settingFor(first.number));
    rpc('setConfig', { config: state.config }).catch((e) => toast(e.message));
  }, 120);
}
function saveLibrary() {
  clearTimeout(libraryTimer);
  libraryTimer = setTimeout(() => {
    const clean = { version: 1, folders: state.library.folders, items: state.library.items.map(({ missing, failed, ...keep }) => keep) };
    rpc('saveLibrary', { library: clean }).catch((e) => toast(e.message));
  }, 300);
}

async function urlFor(path) {
  if (!mediaUrls.has(path)) {
    const [url] = await rpc('mediaUrls', { paths: [path] });
    mediaUrls.set(path, url);
  }
  return mediaUrls.get(path);
}

// ---------- library ----------

function addPaths(paths) {
  let added = 0;
  for (const path of paths) {
    if (itemByPath(path)) continue;
    state.library.items.push({ id: hashId(keyOf(path)), path, name: baseName(path), added: Date.now() + added });
    added++;
  }
  if (added) {
    saveLibrary();
    queueThumbs();
  }
  return added;
}

async function refreshLibraryFiles() {
  const items = state.library.items;
  if (items.length) {
    const exists = await rpc('statFiles', { paths: items.map((i) => i.path) });
    items.forEach((item, i) => { item.missing = !exists[i]; });
  }
  if (state.library.folders.length) addPaths(await rpc('scanFolders', { folders: state.library.folders }));
}

// Thumbnails are captured here from the video itself and stored by the host as small JPEGs.
let thumbWorker = null;
function queueThumbs() {
  if (!thumbWorker) thumbWorker = runThumbs().finally(() => { thumbWorker = null; });
}
async function runThumbs() {
  for (;;) {
    const item = state.library.items.find((i) => !i.thumb && !i.missing && !i.failed);
    if (!item) return;
    try {
      await captureThumb(item);
    } catch {
      item.failed = true;
    }
    saveLibrary();
    refreshItem(item);
  }
}
function waitFor(el, event, ms = 8000) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('timeout')), ms);
    el.addEventListener(event, () => { clearTimeout(t); resolve(); }, { once: true });
    el.addEventListener('error', () => { clearTimeout(t); reject(new Error('media error')); }, { once: true });
  });
}
async function captureThumb(item) {
  const video = document.createElement('video');
  video.muted = true;
  video.preload = 'auto';
  video.crossOrigin = 'anonymous';
  video.src = await urlFor(item.path);
  try {
    await waitFor(video, 'loadeddata');
    item.width = video.videoWidth;
    item.height = video.videoHeight;
    item.duration = video.duration;
    video.currentTime = Math.min(1.5, (video.duration || 0) * 0.15);
    await waitFor(video, 'seeked');

    const scale = 480 / Math.max(video.videoWidth, video.videoHeight);
    const canvas = document.createElement('canvas');
    canvas.width = Math.round(video.videoWidth * scale);
    canvas.height = Math.round(video.videoHeight * scale);
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
    item.accent = vividColor(ctx, canvas.width, canvas.height);
    const url = await rpc('saveThumb', { id: item.id, dataUrl: canvas.toDataURL('image/jpeg', 0.84) });
    item.thumb = `${url}?v=${Date.now().toString(36)}`;
  } finally {
    video.removeAttribute('src');
    video.load();
  }
}

// A representative, vivid color from the frame, lifted to a readable accent.
function vividColor(ctx, w, h) {
  const data = ctx.getImageData(0, 0, w, h).data;
  let r = 0, g = 0, b = 0, total = 0;
  for (let i = 0; i < data.length; i += 16) {
    const R = data[i], G = data[i + 1], B = data[i + 2];
    const max = Math.max(R, G, B), min = Math.min(R, G, B);
    const sat = max ? (max - min) / max : 0;
    const weight = sat * sat * (max / 255) + 0.015;
    r += R * weight; g += G * weight; b += B * weight; total += weight;
  }
  r /= total * 255; g /= total * 255; b /= total * 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
  let hue = 0;
  if (d) hue = max === r ? ((g - b) / d) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  hue = Math.round(((hue * 60) + 360) % 360);
  const sat = d ? d / (1 - Math.abs(max + min - 1)) : 0;
  return `hsl(${hue} ${Math.round(clamp(sat, 0.62, 0.9) * 100)}% 68%)`;
}

// ---------- ambient light ----------

let ambientIndex = 0, ambientKey = '';
function setAmbient() {
  const setting = settingFor(state.selected);
  const item = setting.videos.length ? itemByPath(setting.videos[0]) : null;
  const key = item?.thumb || '';
  document.documentElement.style.setProperty('--accent', item?.accent || '#f4b860');
  if (key === ambientKey) return;
  ambientKey = key;
  const layers = document.querySelectorAll('.ambient__layer');
  const next = layers[(ambientIndex = 1 - ambientIndex)];
  const prev = layers[1 - ambientIndex];
  if (key) next.style.backgroundImage = `url("${key}")`;
  next.classList.toggle('is-on', !!key);
  prev.classList.remove('is-on');
}

// ---------- hover previews ----------

// Plays the real video over a thumbnail only while the pointer is on it.
function attachPreview(container, path, fit = 'cover', brightness = 100) {
  let video = null, timer = 0;
  container.addEventListener('mouseenter', () => {
    timer = setTimeout(async () => {
      const url = await urlFor(path).catch(() => null);
      if (!url || video) return;
      video = document.createElement('video');
      video.muted = true; video.loop = true; video.playsInline = true;
      video.className = container.dataset.videoClass || '';
      video.style.objectFit = fit;
      if (brightness < 100) video.style.filter = `brightness(${brightness}%)`;
      video.src = url;
      container.appendChild(video);
      video.play().catch(() => {});
    }, 180);
  });
  container.addEventListener('mouseleave', () => {
    clearTimeout(timer);
    if (video) { video.pause(); video.removeAttribute('src'); video.load(); video.remove(); video = null; }
  });
}
const objectFit = (fit) => (fit === 'fit' ? 'contain' : fit === 'stretch' ? 'fill' : 'cover');

// ---------- rendering: shell ----------

function render() {
  document.body.dataset.view = state.view;
  for (const tab of document.querySelectorAll('.tab')) tab.setAttribute('aria-selected', String(tab.dataset.view === state.view));
  for (const view of document.querySelectorAll('.view')) view.hidden = view.dataset.view !== state.view;
  renderStatus();
  if (state.view === 'monitors') { renderStage(); renderInspector(); }
  if (state.view === 'library') renderLibrary();
  if (state.view === 'settings') renderSettings();
  setAmbient();
}

function renderStatus() {
  const el = $('#status');
  el.classList.remove('status--paused', 'status--stopped');
  if (!state.playerRunning) {
    el.classList.add('status--stopped');
    el.innerHTML = '<span class="status__dot"></span>Stopped <span class="status__hint">· Start</span>';
    el.title = 'Wallpapers are off. Click to start them.';
  } else if (state.config.paused) {
    el.classList.add('status--paused');
    el.innerHTML = '<span class="status__dot"></span>Paused <span class="status__hint">· Resume</span>';
    el.title = 'Click to resume every monitor.';
  } else {
    el.innerHTML = '<span class="status__dot"></span>Playing <span class="status__hint">· Pause</span>';
    el.title = 'Click to pause every monitor.';
  }
}

function setView(view) {
  if (view !== 'library') state.pick = null;
  state.view = view;
  render();
}

// ---------- rendering: monitors ----------

function renderStage() {
  const stage = $('#stage');
  const ms = state.monitors;
  if (!ms.length) { stage.innerHTML = ''; return; }
  const minX = Math.min(...ms.map((m) => m.x)), minY = Math.min(...ms.map((m) => m.y));
  const maxX = Math.max(...ms.map((m) => m.x + m.width)), maxY = Math.max(...ms.map((m) => m.y + m.height));
  const availW = Math.max(200, stage.clientWidth - 120), availH = Math.max(120, stage.clientHeight - 120);
  const scale = Math.min(availW / (maxX - minX), availH / (maxY - minY), 0.24);
  const gap = 18;

  const screens = ms.map((m) => {
    const s = settingFor(m.number);
    const item = s.videos.length ? itemByPath(s.videos[0]) : null;
    const left = (m.x - minX) * scale + gap / 2, top = (m.y - minY) * scale;
    const w = m.width * scale - gap, h = m.height * scale;
    const inner = item?.thumb
      ? `<img class="screen__img" src="${esc(item.thumb)}" alt="" style="object-fit:${objectFit(s.fit)};${s.brightness < 100 ? `filter:brightness(${s.brightness}%)` : ''}">`
      : `<div class="screen__empty">${s.videos.length ? 'Loading preview' : 'No wallpaper'}</div>`;
    return `
      <button class="screen${m.number === state.selected ? ' is-selected' : ''}" data-action="select-monitor" data-monitor="${m.number}"
              style="left:${left}px;top:${top}px;width:${w}px;height:${h}px" aria-label="Monitor ${m.number}, ${esc(m.name)}">
        <div class="screen__glass" data-video-class="screen__video">${inner}</div>
        <div class="screen__stand"></div>
        <div class="screen__label">
          <span class="screen__num">${m.number}</span>
          <span class="screen__name">${esc(m.name)}</span>
          <span class="screen__res mono">${m.width}×${m.height}</span>
        </div>
      </button>`;
  }).join('');
  stage.innerHTML = `<div class="desk" style="width:${(maxX - minX) * scale}px;height:${(maxY - minY) * scale + 70}px">${screens}</div>`;

  for (const el of stage.querySelectorAll('.screen')) {
    const s = settingFor(Number(el.dataset.monitor));
    if (s.videos.length) attachPreview(el.querySelector('.screen__glass'), s.videos[0], objectFit(s.fit), s.brightness);
  }
}

function renderInspector() {
  const n = state.selected;
  const m = monitorByNumber(n);
  const s = settingFor(n);
  const el = $('#inspector');
  if (!m) { el.innerHTML = ''; return; }

  let showing;
  if (!s.videos.length) {
    showing = `
      <div class="empty-slot">
        <div>Nothing on this monitor yet. It shows your normal Windows wallpaper.</div>
        <button class="btn btn--primary" data-action="pick" data-mode="replace">Choose a wallpaper</button>
      </div>`;
  } else if (s.videos.length === 1) {
    const item = itemByPath(s.videos[0]);
    showing = `
      <div class="feature">
        <div class="feature__media" id="feature-media">
          ${item?.thumb ? `<img src="${esc(item.thumb)}" alt="">` : ''}
        </div>
        <div class="feature__info">
          <div class="feature__name">${esc(item?.name || baseName(s.videos[0]))}</div>
          <div class="feature__meta mono">${esc(itemMeta(item))}</div>
          <button class="btn btn--primary" data-action="pick" data-mode="replace">Change wallpaper</button>
          <button class="btn" data-action="pick" data-mode="add">Add another to rotate</button>
          <button class="btn btn--ghost btn--danger" data-action="clear-monitor">Remove</button>
        </div>
      </div>`;
  } else {
    const rows = s.videos.map((path, i) => {
      const item = itemByPath(path);
      return `
        <div class="rotation__row" draggable="true" data-index="${i}">
          <span class="rotation__grip" aria-hidden="true">⋮⋮</span>
          ${item?.thumb ? `<img class="rotation__thumb" src="${esc(item.thumb)}" alt="">` : '<div class="rotation__thumb"></div>'}
          <div style="min-width:0">
            <div class="rotation__name">${esc(item?.name || baseName(path))}</div>
            <div class="rotation__meta mono">${esc(itemMeta(item))}</div>
          </div>
          <button class="icon-btn" data-action="remove-video" data-index="${i}" aria-label="Remove from rotation" title="Remove from rotation">✕</button>
        </div>`;
    }).join('');
    const minutes = [5, 10, 15, 30, 60, 120, 240];
    if (!minutes.includes(s.rotateMinutes)) minutes.push(s.rotateMinutes);
    showing = `
      <div class="rotation" id="rotation">${rows}</div>
      <div class="rotation__controls">
        <label class="inline-switch">Switch every
          <select class="select" data-action="rotate">
            ${minutes.sort((a, b) => a - b).map((v) => `<option value="${v}"${v === s.rotateMinutes ? ' selected' : ''}>${v < 60 ? `${v} min` : `${v / 60} h`}</option>`).join('')}
          </select>
        </label>
        <span class="inline-switch">Shuffle <button class="switch" role="switch" aria-checked="${s.shuffle}" data-action="shuffle" aria-label="Shuffle"></button></span>
        <button class="btn" style="margin-left:auto" data-action="pick" data-mode="add">Add video</button>
      </div>`;
  }

  const fits = [['fill', 'Fill'], ['fit', 'Fit'], ['stretch', 'Stretch']];
  const bPct = ((s.brightness - 10) / 90) * 100, sPct = ((s.speed - 0.25) / 1.75) * 100;
  el.innerHTML = `
    <section class="panel">
      <div class="panel__head">
        <h2 class="panel__title">Monitor ${n}</h2>
        <span class="panel__sub">${esc(m.name)} · <span class="mono">${m.width}×${m.height}</span>${s.videos.length > 1 ? ` · rotating ${s.videos.length} videos` : ''}</span>
      </div>
      ${showing}
    </section>
    <section class="panel">
      <div class="panel__head"><h2 class="panel__title">Look</h2></div>
      <div class="field field--wide">
        <span class="field__label">Fit</span>
        <div class="segmented" role="group" aria-label="Fit">
          ${fits.map(([v, label]) => `<button data-action="fit" data-value="${v}" aria-pressed="${s.fit === v}">${label}</button>`).join('')}
        </div>
      </div>
      <div class="field">
        <label class="field__label" for="brightness">Brightness</label>
        <input id="brightness" class="range" type="range" min="10" max="100" step="5" value="${s.brightness}" style="--fill:${bPct}%" data-action="brightness">
        <span class="field__value mono" id="brightness-value">${s.brightness}%</span>
      </div>
      <div class="field">
        <label class="field__label" for="speed">Speed</label>
        <input id="speed" class="range" type="range" min="0.25" max="2" step="0.25" value="${s.speed}" style="--fill:${sPct}%" data-action="speed">
        <span class="field__value mono" id="speed-value">${s.speed}×</span>
      </div>
      ${state.monitors.length > 1 ? '<button class="btn apply-all" data-action="apply-all">Use this wallpaper and look on every monitor</button>' : ''}
    </section>`;

  const media = $('#feature-media');
  if (media && s.videos.length) attachPreview(media, s.videos[0], 'cover', 100);
  wireRotationDrag();
}

function wireRotationDrag() {
  const list = $('#rotation');
  if (!list) return;
  let from = -1;
  for (const row of list.querySelectorAll('.rotation__row')) {
    row.addEventListener('dragstart', (e) => { from = Number(row.dataset.index); row.classList.add('is-dragging'); e.dataTransfer.effectAllowed = 'move'; });
    row.addEventListener('dragend', () => row.classList.remove('is-dragging'));
    row.addEventListener('dragover', (e) => { if (from >= 0) { e.preventDefault(); row.classList.add('is-over'); } });
    row.addEventListener('dragleave', () => row.classList.remove('is-over'));
    row.addEventListener('drop', (e) => {
      e.preventDefault();
      e.stopPropagation();
      const to = Number(row.dataset.index);
      if (from < 0 || from === to) return;
      const videos = settingFor(state.selected).videos;
      videos.splice(to, 0, videos.splice(from, 1)[0]);
      from = -1;
      saveConfig();
      renderInspector();
      renderStage();
    });
  }
}

function powerOn(n) {
  const el = document.querySelector(`.screen[data-monitor="${n}"]`);
  if (!el) return;
  el.classList.remove('is-powering');
  void el.offsetWidth; // restart the animation
  el.classList.add('is-powering');
  setTimeout(() => el.classList.remove('is-powering'), 700);
}

function setVideos(n, videos, { undo = true } = {}) {
  const s = settingFor(n);
  const before = [...s.videos];
  s.videos = videos;
  saveConfig();
  if (state.view === 'monitors') { renderStage(); renderInspector(); powerOn(n); }
  setAmbient();
  if (undo) {
    const item = videos.length ? itemByPath(videos[videos.length - 1]) : null;
    toast(item ? `“${item.name}” is on monitor ${n}` : `Monitor ${n} cleared`, {
      action: 'Undo',
      onAction: () => setVideos(n, before, { undo: false }),
    });
  }
}

// ---------- rendering: library ----------

function sortedItems() {
  const q = state.search.trim().toLowerCase();
  let items = state.library.items.filter((i) => !q || i.name.toLowerCase().includes(q));
  if (state.sort === 'name') items.sort((a, b) => a.name.localeCompare(b.name));
  else if (state.sort === 'size') items.sort((a, b) => (b.width || 0) * (b.height || 0) - (a.width || 0) * (a.height || 0));
  else items.sort((a, b) => b.added - a.added);
  return items;
}

function pipsHtml(item) {
  const ms = state.monitors;
  if (!ms.length) return '';
  const minX = Math.min(...ms.map((m) => m.x)), minY = Math.min(...ms.map((m) => m.y));
  const maxX = Math.max(...ms.map((m) => m.x + m.width)), maxY = Math.max(...ms.map((m) => m.y + m.height));
  const scale = Math.min(64 / (maxX - minX), 26 / (maxY - minY));
  let used = false;
  const pips = ms.map((m) => {
    const on = settingFor(m.number).videos.some((p) => keyOf(p) === keyOf(item.path));
    used ||= on;
    const l = (m.x - minX) * scale + 1.5, t = (m.y - minY) * scale, w = m.width * scale - 3, h = m.height * scale;
    return `<button class="pip${on ? ' is-on' : ''}" data-action="pip" data-monitor="${m.number}" data-id="${item.id}"
              style="left:${l}px;top:${t}px;width:${w}px;height:${h}px" title="Show on monitor ${m.number}" aria-label="Show on monitor ${m.number}">${m.number}</button>`;
  }).join('');
  return `<div class="pips${used ? ' is-used' : ''}"><div class="pips__desk" style="width:${(maxX - minX) * scale}px;height:${(maxY - minY) * scale}px">${pips}</div></div>`;
}

function cardHtml(item) {
  const media = item.thumb
    ? `<img src="${esc(item.thumb)}" alt="" loading="lazy">`
    : `<div class="card__placeholder">${item.missing ? '' : item.failed ? 'No preview' : 'Making preview…'}</div>`;
  return `
    <article class="card${item.missing ? ' is-missing' : ''}" data-id="${item.id}" tabindex="0" aria-label="${esc(item.name)}">
      <div class="card__media">
        ${media}
        ${item.missing ? '<span class="card__badge">File not found</span>' : ''}
        ${item.missing ? '' : pipsHtml(item)}
        <button class="card__more" data-action="card-menu" data-id="${item.id}" aria-label="More options" title="More options">⋯</button>
      </div>
      <div class="card__meta">
        <div class="card__name" title="${esc(item.path)}">${esc(item.name)}</div>
        <div class="card__info mono">${esc(itemMeta(item))}</div>
      </div>
    </article>`;
}

function renderLibrary() {
  const items = sortedItems();
  const total = state.library.items.length;
  $('#lib-count').textContent = total ? `${total} video${total === 1 ? '' : 's'}` : '';
  $('#lib-empty').hidden = total > 0;
  $('#grid').hidden = total === 0;

  const pickbar = $('#pickbar');
  pickbar.hidden = !state.pick;
  if (state.pick) {
    pickbar.innerHTML = `
      <span class="pickbar__text">${state.pick.mode === 'add' ? `Choose a video to add to monitor ${state.pick.monitor}'s rotation` : `Choose a wallpaper for monitor ${state.pick.monitor}`}</span>
      <button class="btn" data-action="cancel-pick">Cancel</button>`;
  }

  const grid = $('#grid');
  grid.innerHTML = items.length || !total ? items.map(cardHtml).join('') : '<p class="panel__sub">No videos match your search.</p>';
  for (const card of grid.querySelectorAll('.card')) {
    const item = state.library.items.find((i) => i.id === card.dataset.id);
    if (item && !item.missing) attachPreview(card.querySelector('.card__media'), item.path);
  }
}

function refreshItem(item) {
  if (state.view === 'library') {
    const card = document.querySelector(`.card[data-id="${item.id}"]`);
    if (card) {
      card.outerHTML = cardHtml(item);
      const fresh = document.querySelector(`.card[data-id="${item.id}"]`);
      if (!item.missing) attachPreview(fresh.querySelector('.card__media'), item.path);
    }
  }
  const inUse = state.monitors.some((m) => settingFor(m.number).videos.some((p) => keyOf(p) === keyOf(item.path)));
  if (inUse && state.view === 'monitors') { renderStage(); renderInspector(); }
  if (inUse) setAmbient();
}

function applyFromLibrary(item, monitor = state.selected, mode = 'replace') {
  if (item.missing) return toast('That file was moved or deleted. Remove it from the library or add it again.');
  const s = settingFor(monitor);
  if (mode === 'add') {
    if (s.videos.some((p) => keyOf(p) === keyOf(item.path))) return toast('That video is already in the rotation');
    setVideos(monitor, [...s.videos, item.path]);
  } else {
    setVideos(monitor, [item.path]);
  }
}

// ---------- rendering: settings ----------

function renderSettings() {
  const c = state.config;
  const caps = [[0, 'Off'], [15, '15'], [24, '24'], [30, '30'], [60, '60']];
  const chips = c.pauseFor.map((exe) => `<span class="chip">${esc(exe)}<button class="icon-btn" data-action="unpause-app" data-exe="${esc(exe)}" aria-label="Stop pausing for ${esc(exe)}">✕</button></span>`).join('');
  const folders = state.library.folders.map((f) => `
    <div class="folder"><span class="folder__path mono" title="${esc(f)}">${esc(f)}</span>
      <button class="btn btn--ghost btn--danger" data-action="remove-folder" data-folder="${esc(f)}">Remove</button></div>`).join('');
  const sw = (key, on, label) => `<button class="switch" role="switch" aria-checked="${on}" data-action="toggle" data-key="${key}" aria-label="${esc(label)}"></button>`;

  $('#view-settings').innerHTML = `
    <div class="settings">
      <h1 class="title">Settings</h1>
      <section class="panel">
        <div class="panel__head"><h2 class="panel__title">Performance</h2></div>
        <div class="row">
          <div class="row__text">
            <div class="row__title">Frame-rate cap</div>
            <div class="row__desc">Shows at most this many frames per second. Fewer frames means less GPU work; slow scenes look the same at 24–30.</div>
          </div>
          <div class="segmented" role="group" aria-label="Frame-rate cap">
            ${caps.map(([v, label]) => `<button data-action="fps" data-value="${v}" aria-pressed="${c.fpsCap === v}">${label}</button>`).join('')}
          </div>
        </div>
      </section>
      <section class="panel">
        <div class="panel__head"><h2 class="panel__title">When to pause</h2></div>
        <div class="row">
          <div class="row__text"><div class="row__title">Pause a monitor when windows cover it</div>
            <div class="row__desc">Nobody can see it, so it stops decoding and drawing until you uncover it.</div></div>
          ${sw('pauseWhenCovered', c.pauseWhenCovered, 'Pause a monitor when windows cover it')}
        </div>
        <div class="row">
          <div class="row__text"><div class="row__title">Pause every monitor during fullscreen games and videos</div>
            <div class="row__desc">Frees the GPU completely while a fullscreen app has focus, even on other monitors.</div></div>
          ${sw('pauseAllWhenFullscreen', c.pauseAllWhenFullscreen, 'Pause every monitor during fullscreen games and videos')}
        </div>
        <div class="row" style="align-items:flex-start">
          <div class="row__text"><div class="row__title">Pause while these apps are running</div>
            <div class="row__desc">Every monitor pauses as soon as one of them starts, whether or not it's on screen.</div>
            ${chips ? `<div class="chips">${chips}</div>` : ''}</div>
          <button class="btn" data-action="add-app">Add app</button>
        </div>
      </section>
      <section class="panel">
        <div class="panel__head"><h2 class="panel__title">Library folders</h2></div>
        <div class="row__desc" style="margin-bottom:8px">New videos in these folders are added to your library each time you open Wallpaper Plus.</div>
        ${folders || '<div class="row__desc">No folders yet.</div>'}
        <button class="btn" style="margin-top:10px" data-action="add-folder">Add folder</button>
      </section>
      <section class="panel">
        <div class="panel__head"><h2 class="panel__title">General</h2></div>
        <div class="row">
          <div class="row__text"><div class="row__title">Start with Windows</div>
            <div class="row__desc">Wallpapers start playing when you sign in.</div></div>
          ${sw('autostart', c.autostart, 'Start with Windows')}
        </div>
        <div class="row">
          <div class="row__text"><div class="row__title">Wallpapers ${state.playerRunning ? 'are running' : 'are stopped'}</div>
            <div class="row__desc">${state.playerRunning ? 'Stopping them brings back your normal Windows wallpaper until you start them again.' : 'Your normal Windows wallpaper is showing.'}</div></div>
          ${state.playerRunning ? '<button class="btn" data-action="stop-player">Stop wallpapers</button>' : '<button class="btn btn--primary" data-action="start-player">Start wallpapers</button>'}
        </div>
      </section>
    </div>`;
}

// ---------- menus & toasts ----------

let menuClose = null;
function openMenu(x, y, entries) {
  const menu = $('#menu');
  menu.innerHTML = entries.map((e, i) =>
    e === '-' ? '<div class="menu__sep"></div>'
      : e.label && !e.run ? `<div class="menu__label">${esc(e.label)}</div>`
      : `<button class="menu__item${e.danger ? ' menu__item--danger' : ''}" data-index="${i}" role="menuitem">${esc(e.label)}</button>`).join('');
  menu.hidden = false;
  const r = menu.getBoundingClientRect();
  menu.style.left = `${clamp(x, 8, innerWidth - r.width - 8)}px`;
  menu.style.top = `${clamp(y, 8, innerHeight - r.height - 8)}px`;
  menu.querySelector('.menu__item')?.focus();
  menu.onclick = (e) => {
    const btn = e.target.closest('.menu__item');
    if (!btn) return;
    closeMenu();
    entries[Number(btn.dataset.index)].run();
  };
  setTimeout(() => {
    menuClose = (e) => { if (!menu.contains(e.target)) closeMenu(); };
    document.addEventListener('mousedown', menuClose);
  });
}
function closeMenu() {
  $('#menu').hidden = true;
  if (menuClose) document.removeEventListener('mousedown', menuClose);
  menuClose = null;
}

let toastTimer = 0;
function toast(text, { action, onAction } = {}) {
  const el = $('#toast');
  el.innerHTML = `<span>${esc(text)}</span>${action ? `<button class="btn">${esc(action)}</button>` : ''}`;
  if (action) el.querySelector('button').onclick = () => { el.classList.remove('is-on'); onAction(); };
  el.classList.add('is-on');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('is-on'), action ? 5000 : 3000);
}

// ---------- actions ----------

async function addVideosDialog() {
  const paths = await rpc('pickVideos');
  const n = addPaths(paths);
  if (paths.length) toast(n ? `Added ${n} video${n === 1 ? '' : 's'}` : 'Those videos are already in your library');
  if (state.view === 'library') renderLibrary();
}

async function addFolderDialog() {
  const res = await rpc('pickFolder');
  if (!res) return;
  if (!state.library.folders.some((f) => keyOf(f) === keyOf(res.folder))) state.library.folders.push(res.folder);
  const n = addPaths(res.videos);
  saveLibrary();
  toast(res.videos.length ? `Added ${n} video${n === 1 ? '' : 's'} from the folder` : 'No videos in that folder yet. New ones are added when you open Wallpaper Plus.');
  render();
}

function removeItem(item) {
  const users = state.monitors.filter((m) => settingFor(m.number).videos.some((p) => keyOf(p) === keyOf(item.path)));
  for (const m of users) settingFor(m.number).videos = settingFor(m.number).videos.filter((p) => keyOf(p) !== keyOf(item.path));
  if (users.length) saveConfig();
  state.library.items = state.library.items.filter((i) => i !== item);
  rpc('deleteThumb', { id: item.id }).catch(() => {});
  saveLibrary();
  render();
  toast(users.length ? `Removed from your library and from monitor ${users.map((m) => m.number).join(' and ')}` : 'Removed from your library');
}

function cardMenu(item, x, y) {
  const entries = [];
  if (!item.missing) {
    for (const m of state.monitors) entries.push({ label: `Show on monitor ${m.number}`, run: () => applyFromLibrary(item, m.number) });
    if (state.monitors.length > 1) entries.push({ label: 'Show on every monitor', run: () => state.monitors.forEach((m) => applyFromLibrary(item, m.number)) });
    entries.push('-');
    for (const m of state.monitors) entries.push({ label: `Add to monitor ${m.number}'s rotation`, run: () => applyFromLibrary(item, m.number, 'add') });
    entries.push('-');
  }
  entries.push({ label: 'Show in File Explorer', run: () => rpc('showInExplorer', { path: item.path }) });
  entries.push({ label: 'Remove from library', danger: true, run: () => removeItem(item) });
  openMenu(x, y, entries);
}

async function addAppMenu(x, y) {
  const apps = await rpc('runningApps').catch(() => []);
  const entries = [];
  const fresh = apps.filter((a) => !state.config.pauseFor.includes(a.exe));
  if (fresh.length) {
    entries.push({ label: 'Running now' });
    for (const a of fresh.slice(0, 12)) entries.push({ label: `${a.exe} — ${a.title.slice(0, 40)}`, run: () => addPauseApp(a.exe) });
    entries.push('-');
  }
  entries.push({ label: 'Browse for a program…', run: async () => { const exe = await rpc('pickExe'); if (exe) addPauseApp(exe); } });
  openMenu(x, y, entries);
}
function addPauseApp(exe) {
  exe = exe.toLowerCase();
  if (!state.config.pauseFor.includes(exe)) state.config.pauseFor.push(exe);
  saveConfig();
  renderSettings();
  toast(`Wallpapers pause while ${exe} is running`);
}

async function pollPlayer() {
  const running = await rpc('playerRunning').catch(() => state.playerRunning);
  if (running !== state.playerRunning) {
    state.playerRunning = running;
    renderStatus();
    if (state.view === 'settings') renderSettings();
  }
}

document.addEventListener('click', async (e) => {
  const tab = e.target.closest('.tab');
  if (tab) return setView(tab.dataset.view);

  const el = e.target.closest('[data-action]');
  const card = e.target.closest('.card');
  if (!el && card) { // plain click on a library card
    const item = state.library.items.find((i) => i.id === card.dataset.id);
    if (!item) return;
    if (state.pick) {
      const { monitor, mode } = state.pick;
      state.pick = null;
      state.selected = monitor;
      state.view = 'monitors';
      render();
      applyFromLibrary(item, monitor, mode);
    } else {
      applyFromLibrary(item, state.selected);
      renderLibrary();
    }
    return;
  }
  if (!el) return;

  const s = settingFor(state.selected);
  const action = el.dataset.action;
  try {
    switch (action) {
      case 'status':
        if (!state.playerRunning) {
          await rpc('startPlayer');
          toast('Starting wallpapers');
          setTimeout(pollPlayer, 1500);
        } else {
          state.config.paused = !state.config.paused;
          saveConfig();
          renderStatus();
          toast(state.config.paused ? 'Every monitor is paused' : 'Wallpapers resumed');
        }
        break;
      case 'select-monitor':
        state.selected = Number(el.dataset.monitor);
        renderStage(); renderInspector(); setAmbient();
        break;
      case 'pick':
        state.pick = { monitor: state.selected, mode: el.dataset.mode };
        state.view = 'library';
        render();
        break;
      case 'cancel-pick':
        setView('monitors');
        break;
      case 'clear-monitor':
        setVideos(state.selected, []);
        break;
      case 'remove-video':
        setVideos(state.selected, s.videos.filter((_, i) => i !== Number(el.dataset.index)));
        break;
      case 'fit':
        s.fit = el.dataset.value;
        saveConfig(); renderStage(); renderInspector();
        break;
      case 'shuffle':
        s.shuffle = !s.shuffle;
        saveConfig(); renderInspector();
        break;
      case 'apply-all':
        for (const m of state.monitors) if (m.number !== state.selected) state.config.monitors[m.number] = structuredClone(s);
        saveConfig(); renderStage();
        state.monitors.forEach((m) => powerOn(m.number));
        toast('Every monitor now matches monitor ' + state.selected);
        break;
      case 'pip': {
        e.stopPropagation();
        const item = state.library.items.find((i) => i.id === el.dataset.id);
        const n = Number(el.dataset.monitor);
        const on = settingFor(n).videos.some((p) => keyOf(p) === keyOf(item.path));
        if (!on) applyFromLibrary(item, n);
        renderLibrary();
        break;
      }
      case 'card-menu': {
        e.stopPropagation();
        const item = state.library.items.find((i) => i.id === el.dataset.id);
        const r = el.getBoundingClientRect();
        cardMenu(item, r.right - 220, r.bottom + 6);
        break;
      }
      case 'add-videos': await addVideosDialog(); break;
      case 'add-folder': await addFolderDialog(); break;
      case 'remove-folder':
        state.library.folders = state.library.folders.filter((f) => f !== el.dataset.folder);
        saveLibrary(); renderSettings();
        toast('Folder removed. Videos already in your library stay.');
        break;
      case 'fps':
        state.config.fpsCap = Number(el.dataset.value);
        saveConfig(); renderSettings();
        break;
      case 'toggle':
        state.config[el.dataset.key] = !state.config[el.dataset.key];
        saveConfig(); renderSettings();
        break;
      case 'add-app': {
        const r = el.getBoundingClientRect();
        await addAppMenu(r.right - 320, r.bottom + 6);
        break;
      }
      case 'unpause-app':
        state.config.pauseFor = state.config.pauseFor.filter((x) => x !== el.dataset.exe);
        saveConfig(); renderSettings();
        break;
      case 'stop-player':
        await rpc('quitPlayer');
        toast('Wallpapers stopped');
        setTimeout(pollPlayer, 800);
        break;
      case 'start-player':
        await rpc('startPlayer');
        toast('Starting wallpapers');
        setTimeout(pollPlayer, 1500);
        break;
      case 'win':
        await rpc('window', { action: el.dataset.win });
        break;
    }
  } catch (err) {
    toast(err.message);
  }
});

document.addEventListener('input', (e) => {
  const el = e.target;
  const s = settingFor(state.selected);
  if (el.dataset.action === 'brightness') {
    s.brightness = Number(el.value);
    el.style.setProperty('--fill', `${((s.brightness - 10) / 90) * 100}%`);
    $('#brightness-value').textContent = `${s.brightness}%`;
    const img = document.querySelector(`.screen[data-monitor="${state.selected}"] .screen__img`);
    if (img) img.style.filter = s.brightness < 100 ? `brightness(${s.brightness}%)` : '';
    saveConfig();
  } else if (el.dataset.action === 'speed') {
    s.speed = Number(el.value);
    el.style.setProperty('--fill', `${((s.speed - 0.25) / 1.75) * 100}%`);
    $('#speed-value').textContent = `${s.speed}×`;
    saveConfig();
  } else if (el.id === 'search') {
    state.search = el.value;
    renderLibrary();
  }
});

document.addEventListener('change', (e) => {
  const el = e.target;
  if (el.dataset.action === 'rotate') {
    settingFor(state.selected).rotateMinutes = Number(el.value);
    saveConfig();
  } else if (el.id === 'sort') {
    state.sort = el.value;
    renderLibrary();
  }
});

document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') {
    if (!$('#menu').hidden) return closeMenu();
    if (state.pick) return setView('monitors');
  }
  const card = e.target.closest?.('.card');
  if (card && (e.key === 'Enter' || e.key === ' ') && e.target === card) { e.preventDefault(); card.click(); }
});

document.addEventListener('contextmenu', (e) => {
  const card = e.target.closest('.card');
  e.preventDefault();
  if (!card) return;
  const item = state.library.items.find((i) => i.id === card.dataset.id);
  if (item) cardMenu(item, e.clientX, e.clientY);
});

// Dropping files anywhere adds them to the library.
let dragDepth = 0;
const isFileDrag = (e) => [...(e.dataTransfer?.types || [])].includes('Files');
document.addEventListener('dragenter', (e) => { if (isFileDrag(e)) { dragDepth++; $('#dropzone').hidden = false; } });
document.addEventListener('dragleave', (e) => { if (isFileDrag(e) && --dragDepth <= 0) { dragDepth = 0; $('#dropzone').hidden = true; } });
document.addEventListener('dragover', (e) => { if (isFileDrag(e)) e.preventDefault(); });
document.addEventListener('drop', (e) => {
  if (!isFileDrag(e)) return;
  e.preventDefault();
  dragDepth = 0;
  $('#dropzone').hidden = true;
  const files = [...e.dataTransfer.files];
  if (files.length && hasHost) window.chrome.webview.postMessageWithAdditionalObjects({ cmd: 'drop' }, files);
});
onHost('dropped', (paths) => {
  if (!paths.length) return toast('Only video files can be added (MP4, MKV, WebM, MOV, AVI, WMV)');
  const n = addPaths(paths);
  toast(n ? `Added ${n} video${n === 1 ? '' : 's'}` : 'Those videos are already in your library');
  if (state.view === 'library') renderLibrary();
  else if (state.view === 'monitors' && n) setView('library');
});

let resizeTimer = 0;
addEventListener('resize', () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => { if (state.view === 'monitors') renderStage(); }, 60);
});

// ---------- start ----------

async function init() {
  rpc('windowState').then(applyWindowState).catch(() => {});
  const s = await rpc('getState');
  state.monitors = s.monitors;
  state.config = s.config;
  state.playerRunning = s.playerRunning;
  const lib = s.library || {};
  state.library = { version: 1, folders: lib.folders || [], items: Array.isArray(lib.items) ? lib.items : [] };
  state.selected = (s.monitors.find((m) => m.primary) || s.monitors[0] || { number: 1 }).number;
  for (const m of state.monitors) settingFor(m.number);

  // Videos already set as wallpapers (e.g. from a hand-edited settings file) join the library.
  const referenced = state.monitors.flatMap((m) => settingFor(m.number).videos);
  addPaths(referenced);
  render();

  await refreshLibraryFiles().catch(() => {});
  render();
  queueThumbs();
  setInterval(pollPlayer, 4000);
}

init().catch((e) => toast(`Couldn't load settings: ${e.message}`));
