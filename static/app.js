// frontend client for system blackbox dashboard

// dom references for gauges, tables, and badge elements
const elCpuVal = document.getElementById('cpuVal');
const elCpuBar = document.getElementById('cpuBar');

const elRamVal = document.getElementById('ramVal');
const elRamBar = document.getElementById('ramBar');

const elTempVal = document.getElementById('tempVal');
const elTempBar = document.getElementById('tempBar');

const elFanVal = document.getElementById('fanVal');
const elFanBar = document.getElementById('fanBar');

const elLastUpdated = document.getElementById('lastUpdated');
const elEngineBadge = document.getElementById('engineBadge');
const elEngineStatus = document.getElementById('engineStatus');
const elLivePulse = document.getElementById('livePulse');

const elDeletionsBody = document.getElementById('deletionsTableBody');
const elDeletionCount = document.getElementById('deletionCount');

const elAlertsBody = document.getElementById('alertsTableBody');
const elAlertCount = document.getElementById('alertCount');

const elSessionsBody = document.getElementById('sessionsTableBody');
const elSessionCount = document.getElementById('sessionCount');

// cached canvas 2d contexts and data signatures to skip redundant DOM/canvas redraws
const sparkContexts = {};
let lastDeletionsSig = null;
let lastAlertsSig = null;
let lastSessionsSig = null;
let lastHistorySig = null;

// toggles visible panel between files, alerts, and sessions
function switchTab(tab) {
  const tabs = ['files', 'alerts', 'sessions'];
  tabs.forEach(t => {
    const btn = document.getElementById(`tab${capitalize(t)}Btn`);
    const panel = document.getElementById(`panel${capitalize(t)}`);
    if (btn && panel) {
      if (t === tab) {
        btn.classList.add('active');
        panel.style.display = 'block';
      } else {
        btn.classList.remove('active');
        panel.style.display = 'none';
      }
    }
  });
}

// helper to uppercase first character for element ids
function capitalize(s) {
  return s.charAt(0).toUpperCase() + s.slice(1);
}

// renders mini time-series line chart onto canvas
function drawSparkline(canvasId, data, maxVal, strokeColor, fillColor) {
  const canvas = document.getElementById(canvasId);
  if (!canvas) return;
  if (!sparkContexts[canvasId]) {
    sparkContexts[canvasId] = canvas.getContext('2d');
  }
  const ctx = sparkContexts[canvasId];
  const w = canvas.width;
  const h = canvas.height;

  ctx.clearRect(0, 0, w, h);
  if (!data || data.length < 2) return;

  const safeMax = maxVal > 0 ? maxVal : 1;
  const step = w / (data.length - 1);
  const clamp = (v) => Math.max(0, Math.min(safeMax, v));
  const getY = (v) => h - (clamp(v) / safeMax) * (h - 6) - 3;

  // stroke line path
  ctx.beginPath();
  ctx.moveTo(0, getY(data[0]));
  for (let i = 1; i < data.length; i++) {
    ctx.lineTo(i * step, getY(data[i]));
  }

  ctx.strokeStyle = strokeColor;
  ctx.lineWidth = 1.8;
  ctx.stroke();

  // area fill under the line
  ctx.lineTo((data.length - 1) * step, h);
  ctx.lineTo(0, h);
  ctx.closePath();
  ctx.fillStyle = fillColor;
  ctx.fill();
}

// updates gauges and status badge from vitals payload
function renderVitals(data) {
  if (!data) return;

  // engine status indicator
  if (data.cpp_engine_active) {
    elEngineBadge.className = 'status-badge';
    const sessionNote = data.session_id ? ` (Session #${data.session_id})` : '';
    const dbTag = data.db_backend ? ` [${data.db_backend}]` : '';
    elEngineStatus.textContent = `C++ Engine Live${dbTag}${sessionNote}`;
    elLivePulse.style.display = 'block';
  } else {
    elEngineBadge.className = 'status-badge offline';
    const dbTag = data.db_backend ? ` [${data.db_backend}]` : '';
    if (data.timestamp === 'DB Not Found' || data.timestamp === 'DB Not Connected') {
      elEngineStatus.textContent = `DB Disconnected${dbTag}`;
    } else if (data.age_seconds !== null && data.age_seconds !== undefined && data.age_seconds > 4) {
      elEngineStatus.textContent = `Engine Stopped${dbTag} (${data.age_seconds}s ago)`;
    } else {
      elEngineStatus.textContent = `Engine Offline${dbTag}`;
    }
    elLivePulse.style.display = 'none';
  }

  // cpu card
  elCpuVal.textContent = `${Number(data.cpu_usage).toFixed(1)}%`;
  elCpuBar.style.width = `${Math.min(100, Math.max(0, data.cpu_usage))}%`;

  // ram card
  elRamVal.textContent = `${Number(data.ram_usage).toFixed(1)}%`;
  elRamBar.style.width = `${Math.min(100, Math.max(0, data.ram_usage))}%`;

  // temp card
  elTempVal.textContent = `${Number(data.cpu_temp).toFixed(1)} °C`;
  elTempBar.style.width = `${Math.min(100, Math.max(0, data.cpu_temp))}%`;

  // fan card
  const fan = Number(data.fan_speed) || 0;
  elFanVal.textContent = fan > 0 ? `${fan.toLocaleString()} RPM` : '0 RPM';
  elFanBar.style.width = `${Math.min(100, Math.max(0, (fan / 5500) * 100))}%`;

  // timestamp text
  if (data.timestamp) {
    elLastUpdated.textContent = data.timestamp;
  }
}

// marks UI offline when HTTP server cannot be reached
function setServerOffline() {
  elEngineBadge.className = 'status-badge offline';
  elEngineStatus.textContent = 'Server Offline';
  elLivePulse.style.display = 'none';
}

// updates file audit table only when rows actually change
function renderDeletions(data) {
  if (!data) return;
  const items = data.items || [];
  const totalCount = data.total_count ?? items.length;
  elDeletionCount.textContent = `${totalCount}`;

  const sig = items.length === 0
    ? 'empty'
    : `${totalCount}:${items.length}:${items[0].id}:${items[items.length - 1].id}`;
  if (sig === lastDeletionsSig) return;
  lastDeletionsSig = sig;

  if (items.length === 0) {
    elDeletionsBody.innerHTML = `
      <tr>
        <td colspan="4" class="empty-hint">Watching ./test_watch/ for file deletion & move events...</td>
      </tr>
    `;
    return;
  }

  elDeletionsBody.innerHTML = items
    .map(item => {
      let badgeClass = 'ev-deleted';
      if (item.event_type === 'MOVED_OUT') badgeClass = 'ev-moved';
      else if (item.event_type === 'RENAMED') badgeClass = 'ev-renamed';
      else if (item.event_type === 'WATCH_DIR_REMOVED' || item.event_type === 'QUEUE_OVERFLOW') badgeClass = 'ev-special';

      const dirTag = item.is_dir ? '[DIR] ' : '';
      let targetText = `${dirTag}${escapeHtml(item.file_name)}`;
      if (item.new_name) {
        targetText += ` -> <span style="color:#c084fc;">${escapeHtml(item.new_name)}</span>`;
      }

      return `
        <tr>
          <td>#${item.id}</td>
          <td>${escapeHtml(item.timestamp)}</td>
          <td><span class="ev-badge ${badgeClass}">${escapeHtml(item.event_type)}</span></td>
          <td>${targetText}</td>
        </tr>
      `;
    })
    .join('');
}

// updates alerts table and active count badge only when rows actually change
function renderAlerts(data) {
  if (!data) return;
  const items = data.items || [];
  const activeCount = data.active_count ?? 0;
  elAlertCount.textContent = `${activeCount}`;

  const sig = items.length === 0
    ? `empty:${activeCount}`
    : `${activeCount}:${items.length}:${items[0].id}:${items[0].state}:${items[items.length - 1].id}`;
  if (sig === lastAlertsSig) return;
  lastAlertsSig = sig;

  if (items.length === 0) {
    elAlertsBody.innerHTML = `
      <tr>
        <td colspan="6" class="empty-hint">All systems nominal. No threshold breaches recorded.</td>
      </tr>
    `;
    return;
  }

  elAlertsBody.innerHTML = items
    .map(a => {
      const isTriggered = a.state === 'TRIGGERED';
      const stateClass = isTriggered ? 'alert-triggered' : 'alert-resolved';
      return `
        <tr>
          <td>#${a.id}</td>
          <td>${escapeHtml(a.timestamp)}</td>
          <td><code>${escapeHtml(a.metric)}</code></td>
          <td><b>${a.value}</b></td>
          <td>${a.threshold}</td>
          <td class="${stateClass}">${escapeHtml(a.state)}</td>
        </tr>
      `;
    })
    .join('');
}

// updates session history table only when session state changes
function renderSessions(data) {
  if (!data) return;
  const items = data.items || [];
  elSessionCount.textContent = `${items.length}`;

  const sig = items.length === 0
    ? 'empty'
    : items.map(s => `${s.id}:${s.end_time || ''}:${s.clean_exit ?? 'null'}`).join('|');
  if (sig === lastSessionsSig) return;
  lastSessionsSig = sig;

  if (items.length === 0) {
    elSessionsBody.innerHTML = `
      <tr>
        <td colspan="6" class="empty-hint">No session history found.</td>
      </tr>
    `;
    return;
  }

  elSessionsBody.innerHTML = items
    .map(s => {
      let statusHtml = '';
      if (s.clean_exit === 1) {
        statusHtml = `<span class="session-clean">Clean Exit</span>`;
      } else if (s.clean_exit === 0) {
        statusHtml = `<span class="session-crashed">Crashed / Abrupt Termination</span>`;
      } else {
        statusHtml = `<span class="session-active">Active Recording</span>`;
      }
      return `
        <tr>
          <td>#${s.id}</td>
          <td>${escapeHtml(s.start_time)}</td>
          <td>${s.end_time ? escapeHtml(s.end_time) : '<span style="color:#38bdf8;">In Progress</span>'}</td>
          <td><code>PID ${s.pid}</code></td>
          <td>${escapeHtml(s.hostname || '-')}</td>
          <td>${statusHtml}</td>
        </tr>
      `;
    })
    .join('');
}

// repaints sparkline charts only when time-series points advance
function renderHistory(data) {
  if (!data) return;
  const pts = data.points || [];
  const last = pts.length > 0 ? pts[pts.length - 1] : null;
  const sig = last
    ? `${pts.length}:${pts[0].t}:${last.t}:${last.cpu}:${last.ram}:${last.temp}:${last.fan}`
    : 'empty';
  if (sig === lastHistorySig) return;
  lastHistorySig = sig;

  if (pts.length < 2) {
    drawSparkline('cpuSpark', null, 100, '#38bdf8', 'rgba(56, 189, 248, 0.15)');
    drawSparkline('ramSpark', null, 100, '#34d399', 'rgba(52, 211, 153, 0.15)');
    drawSparkline('tempSpark', null, 100, '#fb923c', 'rgba(251, 146, 60, 0.15)');
    drawSparkline('fanSpark', null, 5500, '#a78bfa', 'rgba(167, 139, 250, 0.15)');
    return;
  }

  const cpus = new Array(pts.length);
  const rams = new Array(pts.length);
  const temps = new Array(pts.length);
  const fans = new Array(pts.length);
  let fanMax = 5500;

  for (let i = 0; i < pts.length; i++) {
    const p = pts[i];
    cpus[i] = p.cpu;
    rams[i] = p.ram;
    temps[i] = p.temp;
    fans[i] = p.fan;
    if (p.fan > fanMax) fanMax = p.fan;
  }

  drawSparkline('cpuSpark', cpus, 100, '#38bdf8', 'rgba(56, 189, 248, 0.15)');
  drawSparkline('ramSpark', rams, 100, '#34d399', 'rgba(52, 211, 153, 0.15)');
  drawSparkline('tempSpark', temps, 100, '#fb923c', 'rgba(251, 146, 60, 0.15)');
  drawSparkline('fanSpark', fans, fanMax, '#a78bfa', 'rgba(167, 139, 250, 0.15)');
}

// standalone endpoint helpers preserved for compatibility
async function updateVitals() {
  try {
    const res = await fetch('/api/metrics', { cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    renderVitals(await res.json());
  } catch (err) {
    setServerOffline();
  }
}

async function updateDeletions() {
  try {
    const res = await fetch('/api/deletions', { cache: 'no-store' });
    if (res.ok) renderDeletions(await res.json());
  } catch (err) {
    console.error('Failed to load deletions audit log:', err);
  }
}

async function updateAlerts() {
  try {
    const res = await fetch('/api/alerts', { cache: 'no-store' });
    if (res.ok) renderAlerts(await res.json());
  } catch (err) {
    console.error('Failed to load alerts log:', err);
  }
}

async function updateSessions() {
  try {
    const res = await fetch('/api/sessions', { cache: 'no-store' });
    if (res.ok) renderSessions(await res.json());
  } catch (err) {
    console.error('Failed to load sessions log:', err);
  }
}

async function updateHistory() {
  try {
    const res = await fetch('/api/history?minutes=15', { cache: 'no-store' });
    if (res.ok) renderHistory(await res.json());
  } catch (err) {
    console.error('Failed to load history for sparklines:', err);
  }
}

// fast zero-DOM-allocation HTML escaper for untrusted table cell strings
const HTML_ESCAPE_MAP = {
  '&': '&amp;',
  '<': '&lt;',
  '>': '&gt;',
  '"': '&quot;',
  "'": '&#39;',
};
function escapeHtml(text) {
  if (text === null || text === undefined || text === '') return '';
  return String(text).replace(/[&<>"']/g, ch => HTML_ESCAPE_MAP[ch]);
}

// coordinated single-flight polling loop: batches refreshes via /api/snapshot and pauses when tab is hidden
let pollTimer = null;
let pollInFlight = false;
let tickCounter = 0;

async function pollDashboard(forceFull = false) {
  if (pollInFlight) return;
  pollInFlight = true;
  const full = forceFull || (tickCounter % 2 === 0);
  tickCounter++;

  try {
    const url = full ? '/api/snapshot?full=1&minutes=15' : '/api/snapshot?full=0';
    const res = await fetch(url, { cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const snap = await res.json();

    renderVitals(snap.metrics);
    if (full) {
      renderDeletions(snap.deletions);
      renderAlerts(snap.alerts);
      renderSessions(snap.sessions);
      renderHistory(snap.history);
    }
  } catch (err) {
    setServerOffline();
  } finally {
    pollInFlight = false;
  }
}

function scheduleNextPoll(delayMs = 1000) {
  if (pollTimer !== null) {
    clearTimeout(pollTimer);
    pollTimer = null;
  }
  if (document.hidden) return;
  pollTimer = setTimeout(async () => {
    await pollDashboard(false);
    scheduleNextPoll(1000);
  }, delayMs);
}

document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    if (pollTimer !== null) {
      clearTimeout(pollTimer);
      pollTimer = null;
    }
  } else {
    pollDashboard(true).then(() => scheduleNextPoll(1000));
  }
});

// initial full snapshot fetch on page load
pollDashboard(true).then(() => scheduleNextPoll(1000));
