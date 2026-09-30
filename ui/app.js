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
  library: { version: 1, folders: [], items: [], excluded: [] },
  selected: 1,
  view: 'monitors',
  playerRunning: true,
  decoderStatus: [], // per monitor: which chip is decoding (from the player)
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
const isInside = (path, folder) => keyOf(path).startsWith(keyOf(folder).replace(/[\\/]+$/, '') + '\\');
const isExcluded = (path) => state.library.excluded.includes(keyOf(path));
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
    const clean = { version: 1, folders: state.library.folders, excluded: state.library.excluded, items: state.library.items.map(({ missing, failed, ...keep }) => keep) };
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

// Folder scans skip videos the user removed; adding a video by hand brings it back.
function addPaths(paths, { fromFolderScan = false } = {}) {
  let added = 0;
  for (const path of paths) {
    if (fromFolderScan && isExcluded(path)) continue;
    state.library.excluded = state.library.excluded.filter((k) => k !== keyOf(path));
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

// The host answers these from a background thread, which can take a while when a saved path is on
// a network drive that stopped answering. The page stays usable meanwhile, so the answers are
// applied to what's still there: items removed since are left out, and so are folders.
async function refreshLibraryFiles() {
  const items = [...state.library.items];
  if (items.length) {
    const exists = await rpc('statFiles', { paths: items.map((i) => i.path) });
    items.forEach((item, i) => { item.missing = !exists[i]; });
  }
  if (state.library.folders.length) {
    const found = await rpc('scanFolders', { folders: state.library.folders });
    addPaths(found.filter((p) => state.library.folders.some((f) => isInside(p, f))), { fromFolderScan: true });
  }
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
  if (state.view === 'browse') renderBrowse();
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
  if (view === 'settings') refreshDecoderStatus();
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

// ---------- rendering: browse (motionbgs.com) ----------
// The page never goes online itself (the host refuses every outside request). The host fetches
// listing pages as text, and thumbnails and preview clips into a local cache; this code reads the
// listings and lays them out. A card's preview clip is fetched and looped only while the pointer
// rests on it; moving away tears the video down again.

const SITE = 'https://motionbgs.com';
const TOPICS = [
  ['Latest', '/'], ['4K', '/4k/'], ['Anime', '/tag:anime/'], ['Games', '/tag:games/'], ['Nature', '/tag:nature/'],
  ['Cars', '/tag:car/'], ['Space', '/tag:space/'], ['Rain', '/tag:rain/'], ['Night', '/tag:night/'],
  ['Cyberpunk', '/tag:cyberpunk/'], ['Fantasy', '/tag:fantasy/'], ['Superhero', '/tag:superhero/'],
  ['Japan', '/tag:japan/'], ['Animals', '/tag:animal/'], ['Dark', '/tag:dark/'],
];
const browse = {
  topic: '/',           // chosen topic's page
  query: '',            // search text; while set it replaces the topic
  base: '/',            // numbered pages hang off this ('/tag:rain/' -> '/tag:rain/2/'); null = one page only
  page: 0,              // pages loaded so far
  next: '/',            // next page to load, or null at the end
  items: [],
  byId: new Map(),
  loading: false,
  error: '',
  empty: '',
  gen: 0,               // bumps with every new list; replies meant for an older list are dropped
  started: false,
  downloads: new Map(), // wallpaper id -> { quality, received, total }
};
const webMediaUrls = new Map(); // site path -> promise of the cached copy's URL
let webSearchTimer = 0;
let browseCols = clamp(Number(localStorage.getItem('browseColumns')) || 4, 2, 8); // cards per row

// 4K when any screen is bigger than 1080p and the wallpaper has it.
const wantsUhd = () => state.monitors.some((m) => m.width > 1920 || m.height > 1080);
const bestQuality = (w) => (w.has4k && wantsUhd() ? '4k' : 'hd');
const ownedWeb = (id) => state.library.items.find((i) => i.source?.site === 'motionbgs' && i.source.id === id && !i.missing);

// Listing cards look like <a title="… live wallpaper" href=/slug><img src=/i/c/364x205/media/10050/galaxy-eyes.3840x2160.jpg>
// … <span class=ttl>Galaxy Eyes</span><span class=frm>4K</span></a>. The number in the image path
// is also the download number, and the preview clip sits at /media/<number>/<name>.960x540.mp4.
function parseListing(html) {
  const doc = new DOMParser().parseFromString(html, 'text/html');
  const items = [];
  for (const a of doc.querySelectorAll('a[href][title$=" live wallpaper"]')) {
    const m = a.querySelector('img')?.getAttribute('src')?.match(/^\/i\/c\/(\d+)x\d+\/media\/(\d+)\/([\w.-]+?)(\.\d+x\d+)?\.jpg$/);
    if (!m || Number(m[1]) < 200) continue; // tiny menu icons, not listing cards
    const [, , id, name, res = ''] = m;
    items.push({
      id,
      page: a.getAttribute('href'),
      title: (a.querySelector('.ttl')?.textContent || a.title.replace(/ live wallpaper$/, '')).trim(),
      thumbFile: `${id}/${name}${res}.jpg`,
      preview: `/media/${id}/${name}.960x540.mp4`,
      has4k: /4k/i.test(a.querySelector('.frm')?.textContent || ''),
    });
  }
  return { doc, items };
}

// Requests of the current list share the group "b<gen>"; a card's preview clip gets its own
// subgroup so it can be called off alone when the pointer leaves before it downloads. Urgent
// requests (the clip being hovered) jump ahead of queued thumbnails.
function webMedia(path, group = `b${browse.gen}`, urgent = false) {
  if (!webMediaUrls.has(path)) {
    const url = rpc('web.media', { path, group, urgent });
    webMediaUrls.set(path, url);
    url.catch(() => webMediaUrls.delete(path)); // try again next time it's needed
  }
  return webMediaUrls.get(path);
}

function renderBrowse() {
  if (!browse.started) {
    browse.started = true;
    browseReset();
  } else {
    for (const card of document.querySelectorAll('.card--web')) refreshWebCard(card.dataset.webId);
  }
}

function renderTopics() {
  $('#web-topics').innerHTML = TOPICS.map(([label, path]) =>
    `<button class="topic" data-action="web-topic" data-path="${esc(path)}" aria-pressed="${!browse.query && browse.topic === path}">${esc(label)}</button>`).join('');
}

// Starts a fresh list for the current topic or search.
function browseReset() {
  rpc('web.cancel', { group: `b${browse.gen}` }).catch(() => {});
  browse.gen++;
  Object.assign(browse, { items: [], page: 0, loading: false, error: '', empty: '' });
  browse.byId.clear();
  browse.base = browse.query ? null : browse.topic;
  browse.next = browse.query ? `/search?q=${encodeURIComponent(browse.query)}` : browse.topic;
  const grid = $('#web-grid');
  for (const video of grid.querySelectorAll('video')) stopVideo(video);
  cardObserver.disconnect();
  grid.innerHTML = '';
  $('#view-browse').scrollTop = 0;
  renderTopics();
  browseMore();
}

async function browseMore() {
  if (browse.loading || !browse.next) return;
  const gen = browse.gen, path = browse.next;
  browse.loading = true;
  browse.error = '';
  renderWebFoot();
  try {
    const res = await rpc('web.page', { path });
    if (gen !== browse.gen) return;
    const first = browse.page === 0;
    if (res.status === 404 && !first) { browse.next = null; return; } // walked past the last page
    if (res.status !== 200) throw new Error(`Couldn't load wallpapers (error ${res.status})`);
    const { doc, items } = parseListing(res.html);
    // A search that names a topic ("rain") lands on that topic's page, which has more pages.
    if (first && browse.query && /^\/tag:[^/]+\/$/.test(res.url)) browse.base = res.url;
    browse.page++;
    const nextPath = browse.base && `${browse.base}${browse.page + 1}/`;
    browse.next = nextPath && doc.querySelector(`a[href="${nextPath}"]`) ? nextPath : null;
    const fresh = items.filter((w) => !browse.byId.has(w.id));
    for (const w of fresh) { browse.byId.set(w.id, w); browse.items.push(w); }
    if (first && !browse.items.length) {
      browse.empty = browse.query && /no results/i.test(doc.querySelector('h1')?.textContent || '')
        ? `No wallpapers match “${browse.query}”.`
        : "Couldn't read this list of wallpapers. The site they come from may have changed its layout.";
    }
    appendWebCards(fresh);
  } catch (e) {
    if (gen === browse.gen) browse.error = e.message;
  } finally {
    if (gen === browse.gen) { browse.loading = false; renderWebFoot(); }
  }
}

// Loads the next page once the bottom of the list comes within a screen or so.
function maybeMoreWeb() {
  const view = $('#view-browse');
  if (state.view === 'browse' && view.scrollHeight - view.scrollTop - view.clientHeight < 900) browseMore();
}

function renderWebFoot() {
  const foot = $('#web-foot');
  if (browse.loading) foot.innerHTML = '<div class="web-foot__spin" role="status" aria-label="Loading"></div>';
  else if (browse.error) foot.innerHTML = `<p>${esc(browse.error)}</p><button class="btn" data-action="web-retry">Try again</button>`;
  else if (browse.empty) foot.innerHTML = `<p>${esc(browse.empty)}</p>`;
  else if (!browse.next && browse.items.length) foot.innerHTML = `<p class="mono">${browse.items.length} wallpaper${browse.items.length === 1 ? '' : 's'} · end of the list</p>`;
  else foot.innerHTML = '';
  if (!browse.loading && browse.next && !browse.error) requestAnimationFrame(maybeMoreWeb);
}

function webStateHtml(w) {
  const dl = browse.downloads.get(w.id);
  if (dl) {
    return `<div class="webcard__progress" style="--p:${dl.total ? (dl.received / dl.total) * 100 : 0}%">
        <span class="mono">${webProgressText(dl)}</span>
        <button class="icon-btn" data-action="web-cancel" data-web-id="${w.id}" aria-label="Cancel download" title="Cancel download">✕</button>
      </div>`;
  }
  if (ownedWeb(w.id)) {
    return `<span class="webcard__owned">In library</span>
      <div class="webcard__actions"><button class="btn btn--primary" data-action="web-apply" data-web-id="${w.id}">Set on monitor ${state.selected}</button></div>`;
  }
  const best = bestQuality(w);
  const get = (q) => `<button class="btn${q === best ? ' btn--primary' : ''}" data-action="web-get" data-web-id="${w.id}" data-quality="${q}">Get ${q.toUpperCase()}</button>`;
  return `<div class="webcard__actions">${w.has4k ? get('4k') + get('hd') : get('hd')}</div>`;
}
function webProgressText(dl) {
  if (dl.total) return `${Math.floor((dl.received / dl.total) * 100)}%`;
  return dl.received ? `${(dl.received / 1048576).toFixed(1)} MB` : 'Waiting…';
}

function webCardHtml(w) {
  return `
    <article class="card card--web" data-web-id="${w.id}" tabindex="0" aria-label="${esc(w.title)}">
      <div class="card__media">
        <div class="card__placeholder"></div>
        <span class="webcard__tag mono">${w.has4k ? '4K' : 'HD'}</span>
        <div class="webcard__state">${webStateHtml(w)}</div>
        <button class="card__more" data-action="web-menu" data-web-id="${w.id}" aria-label="More options" title="More options">⋯</button>
      </div>
      <div class="card__meta"><div class="card__name" title="${esc(w.title)}">${esc(w.title)}</div></div>
    </article>`;
}

function refreshWebCard(id) {
  const w = browse.byId.get(id);
  const slot = document.querySelector(`.card--web[data-web-id="${id}"] .webcard__state`);
  if (w && slot) slot.innerHTML = webStateHtml(w);
}

function appendWebCards(list) {
  const grid = $('#web-grid');
  grid.insertAdjacentHTML('beforeend', list.map(webCardHtml).join(''));
  for (const card of [...grid.children].slice(-list.length)) {
    cardObserver.observe(card);
    card.addEventListener('mouseenter', () => hoverWebCard(card, true));
    card.addEventListener('mouseleave', () => hoverWebCard(card, false));
    card.addEventListener('focusin', () => hoverWebCard(card, true));
    card.addEventListener('focusout', (e) => { if (!card.contains(e.relatedTarget)) hoverWebCard(card, false); });
  }
}

// Thumbnails load a little ahead of scrolling.
const cardObserver = new IntersectionObserver((entries) => {
  for (const e of entries) {
    if (!e.isIntersecting) continue;
    cardObserver.unobserve(e.target);
    loadWebThumb(e.target);
  }
}, { root: $('#view-browse'), rootMargin: '600px 0px' });

// Big cards get the sharper 960-wide image; the rest the 546-wide one.
function webThumbSize() {
  const cardPx = ($('#web-grid').clientWidth / browseCols) * devicePixelRatio;
  return cardPx > 560 ? '960x540' : '546x308';
}

async function loadWebThumb(card) {
  const w = browse.byId.get(card.dataset.webId);
  const size = webThumbSize();
  const current = card.querySelector('.webcard__thumb');
  if (!w || current?.dataset.size === size || current?.dataset.size === '960x540') return; // never trade down
  const path = `/i/c/${size}/media/${w.thumbFile}`;
  const url = await webMedia(path).catch(() => null);
  if (!url || !card.isConnected) return;
  const img = new Image();
  img.alt = '';
  img.className = 'webcard__thumb';
  img.dataset.size = size;
  img.onerror = () => webMediaUrls.delete(path); // trimmed from the cache meanwhile: fetch again next time
  img.onload = () => {
    img.classList.add('is-on');
    if (old) setTimeout(() => old.remove(), 360); // the sharper copy fades in over the old one
  };
  img.src = url;
  const old = card.querySelector('.webcard__thumb');
  const placeholder = card.querySelector('.card__placeholder');
  if (placeholder) placeholder.replaceWith(img);
  else (old || card.querySelector('.webcard__tag')).after(img);
}

// A card's preview plays only while the pointer rests on it (or it has keyboard focus). The short
// delay keeps a pointer that's just passing over from fetching anything.
function hoverWebCard(card, on) {
  clearTimeout(card.hoverTimer);
  card.toggleAttribute('data-hover', on);
  if (on) card.hoverTimer = setTimeout(() => startWebPreview(card), 120);
  else stopWebPreview(card);
}

async function startWebPreview(card) {
  const w = browse.byId.get(card.dataset.webId);
  if (!w || card.querySelector('video')) return;
  card.classList.add('is-fetching');
  const url = await webMedia(w.preview, `b${browse.gen}:${w.id}`, true).catch(() => null);
  card.classList.remove('is-fetching');
  // The pointer may have moved on while the clip downloaded.
  if (!url || !card.isConnected || !card.hasAttribute('data-hover') || document.hidden || card.querySelector('video')) return;
  const video = document.createElement('video');
  video.muted = true; video.loop = true; video.playsInline = true;
  video.className = 'webcard__video';
  video.addEventListener('playing', () => video.classList.add('is-on'), { once: true });
  video.addEventListener('error', () => webMediaUrls.delete(w.preview), { once: true }); // see loadWebThumb
  video.src = url;
  card.querySelector('.card__media').insertBefore(video, card.querySelector('.webcard__tag'));
  video.play().catch(() => {});
}
function stopWebPreview(card) {
  card.classList.remove('is-fetching');
  const video = card.querySelector('video');
  if (video) stopVideo(video);
  else rpc('web.cancel', { group: `b${browse.gen}:${card.dataset.webId}` }).catch(() => {}); // still queued? drop it
}
// Detached videos keep decoding unless their source is dropped.
function stopVideo(video) {
  video.pause();
  video.removeAttribute('src');
  video.load();
  video.remove();
}

function runWebSearch(text) {
  clearTimeout(webSearchTimer);
  const q = text.trim();
  if (q === browse.query) return;
  browse.query = q;
  browseReset();
}

async function webDownload(w, quality = bestQuality(w)) {
  if (browse.downloads.has(w.id)) return;
  browse.downloads.set(w.id, { quality, received: 0, total: 0 });
  refreshWebCard(w.id);
  try {
    const { path } = await rpc('web.download', { id: w.id, quality });
    // The host never saves over an existing file, so an entry already at this path is left over
    // from a video that was deleted: the new wallpaper gets a fresh entry in its place.
    state.library.items = state.library.items.filter((i) => keyOf(i.path) !== keyOf(path));
    addPaths([path]);
    const item = itemByPath(path);
    item.name = w.title;
    item.source = { site: 'motionbgs', id: w.id, quality, page: w.page };
    saveLibrary();
    toast(`“${w.title}” is in your library`, { action: `Set on monitor ${state.selected}`, onAction: () => applyFromLibrary(item, state.selected) });
  } catch (e) {
    if (e.message !== 'cancelled') toast(`Couldn't download “${w.title}”: ${e.message}`);
  } finally {
    browse.downloads.delete(w.id);
    refreshWebCard(w.id);
  }
}

onHost('download', (d) => {
  const dl = browse.downloads.get(d.id);
  if (!dl) return;
  dl.received = d.received;
  dl.total = d.total;
  const bar = document.querySelector(`.card--web[data-web-id="${d.id}"] .webcard__progress`);
  if (!bar) return;
  bar.style.setProperty('--p', `${dl.total ? (dl.received / dl.total) * 100 : 0}%`);
  bar.querySelector('span').textContent = webProgressText(dl);
});

function webCardClick(card) {
  const w = browse.byId.get(card.dataset.webId);
  if (!w || browse.downloads.has(w.id)) return;
  const owned = ownedWeb(w.id);
  if (owned) applyFromLibrary(owned, state.selected);
  else webDownload(w);
}

function webMenu(w, x, y) {
  const entries = [];
  const owned = ownedWeb(w.id);
  if (owned) {
    for (const m of state.monitors) entries.push({ label: `Show on monitor ${m.number}`, run: () => applyFromLibrary(owned, m.number) });
    entries.push('-');
  }
  if (browse.downloads.has(w.id)) {
    entries.push({ label: 'Cancel download', run: () => rpc('web.cancelDownload', { id: w.id }) });
  } else {
    if (w.has4k) entries.push({ label: owned ? 'Download 4K again' : 'Download 4K', run: () => webDownload(w, '4k') });
    entries.push({ label: owned ? 'Download HD again' : 'Download HD', run: () => webDownload(w, 'hd') });
  }
  entries.push('-');
  entries.push({ label: 'Open in browser', run: () => rpc('openExternal', { url: SITE + w.page }) });
  openMenu(x, y, entries);
}

// The slider sets how many cards share a row; fewer means bigger cards. The card at the top of
// the screen stays put while the grid reflows, so dragging doesn't lose your place.
function setBrowseCols(n, { save = true } = {}) {
  const view = $('#view-browse'), grid = $('#web-grid');
  const top = view.getBoundingClientRect().top;
  const anchor = [...grid.children].find((c) => c.getBoundingClientRect().bottom > top);
  const offset = anchor ? anchor.getBoundingClientRect().top - top : 0;
  browseCols = n;
  grid.style.setProperty('--cols', n);
  const input = $('#web-cols');
  input.value = n;
  input.style.setProperty('--fill', `${((n - 2) / 6) * 100}%`);
  $('#web-cols-value').textContent = n;
  if (anchor) view.scrollTop += anchor.getBoundingClientRect().top - top - offset;
  if (save) localStorage.setItem('browseColumns', n);
  // Re-check which thumbnails are near the screen: bigger cards may want sharper images.
  for (const card of grid.children) cardObserver.observe(card);
  maybeMoreWeb();
}
setBrowseCols(browseCols, { save: false });

$('#view-browse').addEventListener('scroll', maybeMoreWeb, { passive: true });
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) return;
  for (const card of document.querySelectorAll('.card--web[data-hover]')) hoverWebCard(card, false);
});

// ---------- rendering: settings ----------

const decoders = [['auto', 'Automatic'], ['power_saving', 'Power saving'], ['cpu', 'Processor']];
const decoderHelp = {
  auto: 'Uses the GPU the monitor is plugged into if it can decode the video, then NVIDIA, then any other GPU, then the processor.',
  power_saving: 'Uses the integrated GPU first, so a laptop\'s NVIDIA or AMD chip can stay asleep.',
  cpu: 'Decodes on the processor and only draws on the GPU. Uses much more processor time; for troubleshooting.',
};

function decoderStatusHtml() {
  if (!state.playerRunning || !state.decoderStatus.length) return '';
  const lines = state.decoderStatus.map((d) => {
    const how = d.hardware ? `${esc(d.device)} hardware decoder` : `Processor (drawn by ${esc(d.device)})`;
    return `<div class="decoder-line"><span class="decoder-line__dot${d.hardware ? '' : ' decoder-line__dot--cpu'}"></span>Monitor ${d.monitor}: ${how} · ${esc(d.codec)} ${esc(d.size)}</div>`;
  });
  return `<div class="decoder-status">${lines.join('')}</div>`;
}

async function refreshDecoderStatus() {
  const list = await rpc('decoderStatus').catch(() => null);
  if (!list || JSON.stringify(list) === JSON.stringify(state.decoderStatus)) return;
  state.decoderStatus = list;
  const box = $('#decoder-status');
  if (box) box.innerHTML = decoderStatusHtml();
}

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
        <div class="row" style="align-items:flex-start">
          <div class="row__text">
            <div class="row__title">Video decoding</div>
            <div class="row__desc">${decoderHelp[c.decoder] || decoderHelp.auto} If the chosen chip can't play a video, the next one takes over, down to the processor, so wallpapers keep playing.</div>
            <div id="decoder-status">${decoderStatusHtml()}</div>
          </div>
          <div class="segmented" role="group" aria-label="Video decoding">
            ${decoders.map(([v, label]) => `<button data-action="decoder" data-value="${v}" aria-pressed="${c.decoder === v}">${label}</button>`).join('')}
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
          <div class="row__text"><div class="row__title">Auto startup on sign in</div>
            <div class="row__desc">Wallpapers start playing when you sign in.</div></div>
          ${sw('autostart', c.autostart, 'Auto startup on sign in')}
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
  const n = addPaths(res.videos, { fromFolderScan: true });
  saveLibrary();
  toast(res.videos.length ? `Added ${n} video${n === 1 ? '' : 's'} from the folder` : 'No videos in that folder yet. New ones are added when you open Wallpaper Plus.');
  render();
}

function removeItem(item) {
  const users = state.monitors.filter((m) => settingFor(m.number).videos.some((p) => keyOf(p) === keyOf(item.path)));
  for (const m of users) settingFor(m.number).videos = settingFor(m.number).videos.filter((p) => keyOf(p) !== keyOf(item.path));
  if (users.length) saveConfig();
  state.library.items = state.library.items.filter((i) => i !== item);
  // Remember the removal, or the next scan of its folder would add it straight back.
  if (state.library.folders.some((f) => isInside(item.path, f)) && !isExcluded(item.path)) state.library.excluded.push(keyOf(item.path));
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
  if (state.view === 'settings') refreshDecoderStatus();
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
  if (!el && card?.dataset.webId) return webCardClick(card);
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
        state.library.excluded = state.library.excluded.filter((k) => state.library.folders.some((f) => isInside(k, f)));
        saveLibrary(); renderSettings();
        toast('Folder removed. Videos already in your library stay.');
        break;
      case 'fps':
        state.config.fpsCap = Number(el.dataset.value);
        saveConfig(); renderSettings();
        break;
      case 'decoder':
        state.config.decoder = el.dataset.value;
        saveConfig(); renderSettings();
        setTimeout(refreshDecoderStatus, 1500);  // the player restarts its monitors with the new choice
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
      case 'web-topic':
        browse.topic = el.dataset.path;
        browse.query = '';
        $('#web-search').value = '';
        clearTimeout(webSearchTimer);
        browseReset();
        break;
      case 'web-get': webDownload(browse.byId.get(el.dataset.webId), el.dataset.quality); break;
      case 'web-cancel': await rpc('web.cancelDownload', { id: el.dataset.webId }); break;
      case 'web-apply': applyFromLibrary(ownedWeb(el.dataset.webId), state.selected); break;
      case 'web-retry': browseMore(); break;
      case 'web-menu': {
        const r = el.getBoundingClientRect();
        webMenu(browse.byId.get(el.dataset.webId), r.right - 220, r.bottom + 6);
        break;
      }
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
  } else if (el.id === 'web-cols') {
    setBrowseCols(Number(el.value));
  } else if (el.id === 'web-search') {
    clearTimeout(webSearchTimer);
    webSearchTimer = setTimeout(() => runWebSearch(el.value), 450);
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
  if (e.key === 'Enter' && e.target.id === 'web-search') return runWebSearch(e.target.value);
  const card = e.target.closest?.('.card');
  if (card && (e.key === 'Enter' || e.key === ' ') && e.target === card) { e.preventDefault(); card.click(); }
});

document.addEventListener('contextmenu', (e) => {
  const card = e.target.closest('.card');
  e.preventDefault();
  if (!card) return;
  if (card.dataset.webId) return webMenu(browse.byId.get(card.dataset.webId), e.clientX, e.clientY);
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
  state.library = { version: 1, folders: lib.folders || [], items: Array.isArray(lib.items) ? lib.items : [], excluded: Array.isArray(lib.excluded) ? lib.excluded : [] };
  state.selected = (s.monitors.find((m) => m.primary) || s.monitors[0] || { number: 1 }).number;
  for (const m of state.monitors) settingFor(m.number);

  // Videos already set as wallpapers (e.g. from a hand-edited settings file) join the library.
  const referenced = state.monitors.flatMap((m) => settingFor(m.number).videos);
  addPaths(referenced);
  render();
  setInterval(pollPlayer, 4000);

  await refreshLibraryFiles().catch(() => {});
  render();
  queueThumbs();
}

init().catch((e) => toast(`Couldn't load settings: ${e.message}`));
