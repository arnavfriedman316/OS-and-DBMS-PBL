/**
 * app.js — System Blackbox Web Telemetry Client
 * Connects to REST endpoints (/api/metrics, /api/deletions, /api/alerts, /api/sessions, /api/history).
 */

// DOM Elements
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

// Tab Switching
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

function capitalize(s) {
  return s.charAt(0).toUpperCase() + s.slice(1);
}

// Sparkline Canvas Renderer
function drawSparkline(canvasId, data, maxVal, strokeColor, fillColor) {
  const canvas = document.getElementById(canvasId);
  if (!canvas || !data || data.length < 2) return;
  const ctx = canvas.getContext('2d');
  const w = canvas.width;
  const h = canvas.height;

  ctx.clearRect(0, 0, w, h);

  const step = w / (data.length - 1);
  const clamp = (v) => Math.max(0, Math.min(maxVal, v));
  const getY = (v) => h - (clamp(v) / maxVal) * (h - 6) - 3;

  // Path
  ctx.beginPath();
  ctx.moveTo(0, getY(data[0]));
  for (let i = 1; i < data.length; i++) {
    ctx.lineTo(i * step, getY(data[i]));
  }

  // Stroke
  ctx.strokeStyle = strokeColor;
  ctx.lineWidth = 1.8;
  ctx.stroke();

  // Gradient area fill
  ctx.lineTo((data.length - 1) * step, h);
  ctx.lineTo(0, h);
  ctx.closePath();
  ctx.fillStyle = fillColor;
  ctx.fill();
}

// 1. Fetch Metrics (1s interval)
async function updateVitals() {
  try {
    const res = await fetch('/api/metrics', { cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();

    // Engine Status
    if (data.cpp_engine_active) {
      elEngineBadge.className = 'status-badge';
      let sessionNote = data.session_id ? ` (Session #${data.session_id})` : '';
      let dbTag = data.db_backend ? ` [${data.db_backend}]` : '';
      elEngineStatus.textContent = `C++ Engine Live${dbTag}${sessionNote}`;
      elLivePulse.style.display = 'block';
    } else {
      elEngineBadge.className = 'status-badge offline';
      let dbTag = data.db_backend ? ` [${data.db_backend}]` : '';
      if (data.timestamp === 'DB Not Found' || data.timestamp === 'DB Not Connected') {
        elEngineStatus.textContent = `DB Disconnected${dbTag}`;
      } else if (data.age_seconds !== null && data.age_seconds > 4) {
        elEngineStatus.textContent = `Engine Stopped${dbTag} (${data.age_seconds}s ago)`;
      } else {
        elEngineStatus.textContent = `Engine Offline${dbTag}`;
      }
      elLivePulse.style.display = 'none';
    }

    // CPU Card
    elCpuVal.textContent = `${Number(data.cpu_usage).toFixed(1)}%`;
    elCpuBar.style.width = `${Math.min(100, Math.max(0, data.cpu_usage))}%`;

    // RAM Card
    elRamVal.textContent = `${Number(data.ram_usage).toFixed(1)}%`;
    elRamBar.style.width = `${Math.min(100, Math.max(0, data.ram_usage))}%`;

    // Temp Card
    elTempVal.textContent = `${Number(data.cpu_temp).toFixed(1)} °C`;
    elTempBar.style.width = `${Math.min(100, Math.max(0, (data.cpu_temp / 100) * 100))}%`;

    // Fan Card
    const fan = Number(data.fan_speed) || 0;
    elFanVal.textContent = fan > 0 ? `${fan.toLocaleString()} RPM` : '0 RPM';
    elFanBar.style.width = `${Math.min(100, Math.max(0, (fan / 5500) * 100))}%`;

    // Timestamp
    if (data.timestamp) {
      elLastUpdated.textContent = data.timestamp;
    }
  } catch (err) {
    elEngineBadge.className = 'status-badge offline';
    elEngineStatus.textContent = 'Server Offline';
    elLivePulse.style.display = 'none';
  }
}

// 2. Fetch File Deletion Events (2s interval)
async function updateDeletions() {
  try {
    const res = await fetch('/api/deletions', { cache: 'no-store' });
    if (!res.ok) return;
    const data = await res.json();
    const items = data.items || [];
    const totalCount = data.total_count || items.length;

    elDeletionCount.textContent = `${totalCount}`;

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
            <td>${item.timestamp}</td>
            <td><span class="ev-badge ${badgeClass}">${escapeHtml(item.event_type)}</span></td>
            <td>${targetText}</td>
          </tr>
        `;
      })
      .join('');
  } catch (err) {
    console.error('Failed to load deletions audit log:', err);
  }
}

// 3. Fetch Alerts (3s interval)
async function updateAlerts() {
  try {
    const res = await fetch('/api/alerts', { cache: 'no-store' });
    if (!res.ok) return;
    const data = await res.json();
    const items = data.items || [];
    elAlertCount.textContent = `${data.active_count || items.length}`;

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
            <td>${a.timestamp}</td>
            <td><code>${escapeHtml(a.metric)}</code></td>
            <td><b>${a.value}</b></td>
            <td>${a.threshold}</td>
            <td class="${stateClass}">${escapeHtml(a.state)}</td>
          </tr>
        `;
      })
      .join('');
  } catch (err) {
    console.error('Failed to load alerts log:', err);
  }
}

// 4. Fetch Sessions (5s interval)
async function updateSessions() {
  try {
    const res = await fetch('/api/sessions', { cache: 'no-store' });
    if (!res.ok) return;
    const data = await res.json();
    const items = data.items || [];
    elSessionCount.textContent = `${items.length}`;

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
            <td>${s.start_time}</td>
            <td>${s.end_time || '<span style="color:#38bdf8;">In Progress</span>'}</td>
            <td><code>PID ${s.pid}</code></td>
            <td>${escapeHtml(s.hostname || '-')}</td>
            <td>${statusHtml}</td>
          </tr>
        `;
      })
      .join('');
  } catch (err) {
    console.error('Failed to load sessions log:', err);
  }
}

// 5. Fetch History for Sparklines (3s interval)
async function updateHistory() {
  try {
    const res = await fetch('/api/history?minutes=15', { cache: 'no-store' });
    if (!res.ok) return;
    const data = await res.json();
    const pts = data.points || [];
    if (pts.length < 2) return;

    drawSparkline('cpuSpark', pts.map(p => p.cpu), 100, '#38bdf8', 'rgba(56, 189, 248, 0.15)');
    drawSparkline('ramSpark', pts.map(p => p.ram), 100, '#34d399', 'rgba(52, 211, 153, 0.15)');
    drawSparkline('tempSpark', pts.map(p => p.temp), 100, '#fb923c', 'rgba(251, 146, 60, 0.15)');
    drawSparkline('fanSpark', pts.map(p => p.fan), 5500, '#a78bfa', 'rgba(167, 139, 250, 0.15)');
  } catch (err) {
    console.error('Failed to load history for sparklines:', err);
  }
}

function escapeHtml(text) {
  if (!text) return '';
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

// Initial Run
updateVitals();
updateDeletions();
updateAlerts();
updateSessions();
updateHistory();

// Periodic intervals
setInterval(updateVitals, 1000);
setInterval(updateDeletions, 2000);
setInterval(updateAlerts, 3000);
setInterval(updateSessions, 5000);
setInterval(updateHistory, 3000);
