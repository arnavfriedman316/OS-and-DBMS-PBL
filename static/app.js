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
  if (!canvas || !data || data.length < 2) return;
  const ctx = canvas.getContext('2d');
  const w = canvas.width;
  const h = canvas.height;

  ctx.clearRect(0, 0, w, h);

  const step = w / (data.length - 1);
  const clamp = (v) => Math.max(0, Math.min(maxVal, v));
  const getY = (v) => h - (clamp(v) / maxVal) * (h - 6) - 3;

  // stroke line path
  ctx.beginPath();
  ctx.moveTo(0, getY(data[0]));
  for (let i = 1; i < data.length; i++) {
    ctx.lineTo(i * step, getY(data[i]));
  }

  ctx.strokeStyle = strokeColor;
  ctx.lineWidth = 1.8;
  ctx.stroke();

  // gradient area fill under the line
  ctx.lineTo((data.length - 1) * step, h);
  ctx.lineTo(0, h);
  ctx.closePath();
  ctx.fillStyle = fillColor;
  ctx.fill();
}

// fetches live telemetry every second and updates gauges and status badge
async function updateVitals() {
  try {
    const res = await fetch('/api/metrics', { cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();

    // engine status indicator
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

    // cpu card
    elCpuVal.textContent = `${Number(data.cpu_usage).toFixed(1)}%`;
    elCpuBar.style.width = `${Math.min(100, Math.max(0, data.cpu_usage))}%`;

    // ram card
    elRamVal.textContent = `${Number(data.ram_usage).toFixed(1)}%`;
    elRamBar.style.width = `${Math.min(100, Math.max(0, data.ram_usage))}%`;

    // temp card
    elTempVal.textContent = `${Number(data.cpu_temp).toFixed(1)} °C`;
    elTempBar.style.width = `${Math.min(100, Math.max(0, (data.cpu_temp / 100) * 100))}%`;

    // fan card
    const fan = Number(data.fan_speed) || 0;
    elFanVal.textContent = fan > 0 ? `${fan.toLocaleString()} RPM` : '0 RPM';
    elFanBar.style.width = `${Math.min(100, Math.max(0, (fan / 5500) * 100))}%`;

    // timestamp text
    if (data.timestamp) {
      elLastUpdated.textContent = data.timestamp;
    }
  } catch (err) {
    elEngineBadge.className = 'status-badge offline';
    elEngineStatus.textContent = 'Server Offline';
    elLivePulse.style.display = 'none';
  }
}

// polls file audit log and redraws table rows
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

// pulls recent threshold breaches and displays active alert count
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

// updates session log and checks for unexpected crashes
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

// fetches historical vitals and repaints sparkline canvas charts
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

// sanitizes untrusted text before injecting into table cells
function escapeHtml(text) {
  if (!text) return '';
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

// initial data fetch on page load
updateVitals();
updateDeletions();
updateAlerts();
updateSessions();
updateHistory();

// periodic polling timers
setInterval(updateVitals, 1000);
setInterval(updateDeletions, 2000);
setInterval(updateAlerts, 3000);
setInterval(updateSessions, 5000);
setInterval(updateHistory, 3000);
