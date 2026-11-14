// frontend client for system blackbox dashboard

// dom references for gauges, tables, chart, and badge elements
const elCpuVal = document.getElementById('cpuVal');
const elCpuBar = document.getElementById('cpuBar');
const elCpuDelta = document.getElementById('cpuDelta');
const elCpuStats = document.getElementById('cpuStats');

const elRamVal = document.getElementById('ramVal');
const elRamBar = document.getElementById('ramBar');
const elRamDelta = document.getElementById('ramDelta');
const elRamStats = document.getElementById('ramStats');

const elTempVal = document.getElementById('tempVal');
const elTempBar = document.getElementById('tempBar');
const elTempDelta = document.getElementById('tempDelta');
const elTempStats = document.getElementById('tempStats');

const elFanVal = document.getElementById('fanVal');
const elFanBar = document.getElementById('fanBar');
const elFanDelta = document.getElementById('fanDelta');
const elFanStats = document.getElementById('fanStats');

const elLastUpdated = document.getElementById('lastUpdated');
const elEngineBadge = document.getElementById('engineBadge');
const elEngineStatus = document.getElementById('engineStatus');
const elLivePulse = document.getElementById('livePulse');

const elDbBackendBadge = document.getElementById('dbBackendBadge');
const elDbBackendLabel = document.getElementById('dbBackendLabel');
const elPipePid = document.getElementById('pipePid');
const elPipeHost = document.getElementById('pipeHost');

const elForensicBanner = document.getElementById('forensicBanner');
const elBannerTitle = document.getElementById('bannerTitle');
const elBannerDetail = document.getElementById('bannerDetail');
const elBannerAlertsBtn = document.getElementById('bannerAlertsBtn');
const elBannerSessionsBtn = document.getElementById('bannerSessionsBtn');

const elMainChart = document.getElementById('mainHistoryChart');
const elMainChartEmpty = document.getElementById('mainChartEmpty');
const elChartTooltip = document.getElementById('chartTooltip');
const elHistoryPointCount = document.getElementById('historyPointCount');

const elDeletionsBody = document.getElementById('deletionsTableBody');
const elDeletionCount = document.getElementById('deletionCount');

const elAlertsBody = document.getElementById('alertsTableBody');
const elAlertCount = document.getElementById('alertCount');

const elSessionsBody = document.getElementById('sessionsTableBody');
const elSessionCount = document.getElementById('sessionCount');

const elFileFilterGroup = document.getElementById('fileFilterGroup');
const elAuditSearchInput = document.getElementById('auditSearchInput');

// cached canvas 2d contexts and data signatures to skip redundant DOM/canvas redraws
const sparkContexts = {};
let lastDeletionsSig = null;
let lastAlertsSig = null;
let lastSessionsSig = null;
let lastHistorySig = null;

// dashboard interactive state
let activeTab = 'files';
let historyMinutes = 15;
let activeChartSeries = 'all';
let fileEventFilter = 'ALL';
let searchQuery = '';

// latest cached API payloads for instant client-side filtering & CSV export
let cachedDeletionsData = { items: [], total_count: 0 };
let cachedAlertsData = { items: [], active_count: 0 };
let cachedSessionsData = { items: [], has_crashed: false };
let latestHistoryPoints = [];
let mainChartState = null;
let hoverPointIdx = -1;
let prevVitalsSnapshot = null;
let dismissedCrashKey = null;
let dismissedAlertKey = null;
try {
  dismissedCrashKey = localStorage.getItem('blackbox_dismissed_crash_key');
  dismissedAlertKey = localStorage.getItem('blackbox_dismissed_alert_key');
} catch (_) {}

function getCurrentCrashKey() {
  const crashed = (cachedSessionsData.items || []).filter(s => s.clean_exit === 0);
  return crashed.length > 0 ? String(crashed[0].id) : 'none';
}

function getCurrentAlertKey() {
  const items = cachedAlertsData.items || [];
  return items.length > 0 ? `${items[0].id}:${cachedAlertsData.active_count || 0}` : 'none';
}

function acknowledgeForensicTab(tab) {
  if (tab === 'sessions') {
    dismissedCrashKey = getCurrentCrashKey();
    try { localStorage.setItem('blackbox_dismissed_crash_key', dismissedCrashKey); } catch (_) {}
  } else if (tab === 'alerts') {
    dismissedAlertKey = getCurrentAlertKey();
    try { localStorage.setItem('blackbox_dismissed_alert_key', dismissedAlertKey); } catch (_) {}
  }
  updateForensicBanner();
}

// toggles visible panel between files, alerts, and sessions
function switchTab(tab) {
  activeTab = tab;
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
  if (elFileFilterGroup) {
    elFileFilterGroup.style.display = tab === 'files' ? 'inline-flex' : 'none';
  }
  if (tab === 'sessions' || tab === 'alerts') {
    acknowledgeForensicTab(tab);
  }
}

// switches tab, dismisses notice banner, and scrolls forensic explorer into view
function jumpToForensicTab(tab) {
  acknowledgeForensicTab(tab);
  switchTab(tab);
  const logCard = document.querySelector('.log-card');
  if (logCard) {
    logCard.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
  }
}

// helper to uppercase first character for element ids
function capitalize(s) {
  return s.charAt(0).toUpperCase() + s.slice(1);
}

// switches history query window and forces refresh
function setHistoryWindow(minutes) {
  historyMinutes = Number(minutes) || 15;
  const btns = document.querySelectorAll('#windowSelector .seg-btn');
  btns.forEach(btn => {
    const m = Number(btn.getAttribute('data-minutes'));
    btn.classList.toggle('active', m === historyMinutes);
  });
  lastHistorySig = null;
  pollDashboard(true);
}

// switches active series filter on main timeline chart
function setChartSeries(series) {
  activeChartSeries = series || 'all';
  const pills = document.querySelectorAll('#seriesSelector .series-pill');
  pills.forEach(pill => {
    pill.classList.toggle('active', pill.getAttribute('data-series') === activeChartSeries);
  });
  renderMainChart(hoverPointIdx);
}

// sets inotify event type filter pill and redraws table immediately
function setFileEventFilter(evType) {
  fileEventFilter = evType || 'ALL';
  const pills = document.querySelectorAll('#fileFilterGroup .filter-pill');
  pills.forEach(pill => {
    pill.classList.toggle('active', pill.getAttribute('data-ev') === fileEventFilter);
  });
  lastDeletionsSig = null;
  renderDeletions(cachedDeletionsData);
}

// renders HiDPI mini time-series line chart onto canvas
function drawSparkline(canvasId, data, maxVal, strokeColor, fillColor, thresholdVal = null) {
  const canvas = document.getElementById(canvasId);
  if (!canvas) return;
  if (!sparkContexts[canvasId]) {
    sparkContexts[canvasId] = canvas.getContext('2d');
  }
  const ctx = sparkContexts[canvasId];
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const cssW = canvas.clientWidth || Number(canvas.getAttribute('width')) || 320;
  const cssH = canvas.clientHeight || Number(canvas.getAttribute('height')) || 40;
  const targetW = Math.round(cssW * dpr);
  const targetH = Math.round(cssH * dpr);

  if (canvas.width !== targetW || canvas.height !== targetH) {
    canvas.width = targetW;
    canvas.height = targetH;
  }

  ctx.save();
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, cssW, cssH);

  if (!data || data.length < 2) {
    ctx.restore();
    return;
  }

  const safeMax = maxVal > 0 ? maxVal : 1;
  const padY = 4;
  const usableH = Math.max(4, cssH - padY * 2);
  const step = cssW / (data.length - 1);
  const clamp = v => Math.max(0, Math.min(safeMax, v));
  const getY = v => cssH - padY - (clamp(v) / safeMax) * usableH;

  if (thresholdVal !== null && thresholdVal > 0 && thresholdVal <= safeMax) {
    const ty = getY(thresholdVal);
    ctx.save();
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.16)';
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 3]);
    ctx.beginPath();
    ctx.moveTo(0, ty);
    ctx.lineTo(cssW, ty);
    ctx.stroke();
    ctx.restore();
  }

  // area fill gradient under the line
  const grad = ctx.createLinearGradient(0, 0, 0, cssH);
  grad.addColorStop(0, fillColor);
  grad.addColorStop(1, 'rgba(0, 0, 0, 0)');

  ctx.beginPath();
  ctx.moveTo(0, getY(data[0]));
  for (let i = 1; i < data.length; i++) {
    ctx.lineTo(i * step, getY(data[i]));
  }
  ctx.lineTo((data.length - 1) * step, cssH);
  ctx.lineTo(0, cssH);
  ctx.closePath();
  ctx.fillStyle = grad;
  ctx.fill();

  // stroke line path
  ctx.beginPath();
  ctx.moveTo(0, getY(data[0]));
  for (let i = 1; i < data.length; i++) {
    ctx.lineTo(i * step, getY(data[i]));
  }
  ctx.strokeStyle = strokeColor;
  ctx.lineWidth = 1.8;
  ctx.lineJoin = 'round';
  ctx.lineCap = 'round';
  ctx.stroke();

  // highlight latest sample endpoint
  const lastX = (data.length - 1) * step;
  const lastY = getY(data[data.length - 1]);
  ctx.beginPath();
  ctx.arc(lastX - 2, lastY, 2.3, 0, Math.PI * 2);
  ctx.fillStyle = strokeColor;
  ctx.fill();

  ctx.restore();
}

// updates delta pill beside a KPI readout
function updateDeltaEl(el, current, prev, unit, precision = 1) {
  if (!el) return;
  if (prev === null || prev === undefined || Number.isNaN(prev)) {
    el.textContent = `0${unit}`;
    el.className = 'kpi-delta flat';
    return;
  }
  const diff = current - prev;
  const eps = precision === 0 ? 1 : 0.05;
  if (Math.abs(diff) < eps) {
    el.textContent = `0${unit}`;
    el.className = 'kpi-delta flat';
    return;
  }
  const sign = diff > 0 ? '+' : '';
  el.textContent = `${sign}${diff.toFixed(precision)}${unit}`;
  el.className = diff > 0 ? 'kpi-delta up' : 'kpi-delta down';
}

// updates min/avg/peak footer text for a KPI card
function updateStatsFooter(el, values, unit, precision = 1) {
  if (!el) return;
  if (!values || values.length === 0) {
    el.textContent = 'MIN — · AVG — · PEAK —';
    return;
  }
  let min = values[0];
  let max = values[0];
  let sum = 0;
  for (let i = 0; i < values.length; i++) {
    const v = values[i];
    if (v < min) min = v;
    if (v > max) max = v;
    sum += v;
  }
  const avg = sum / values.length;
  el.textContent = `MIN ${min.toFixed(precision)}${unit} · AVG ${avg.toFixed(precision)}${unit} · PEAK ${max.toFixed(precision)}${unit}`;
}

function updateHeaderChips(data) {
  const backend = (data && data.db_backend) ? data.db_backend : 'None';

  if (elDbBackendBadge && elDbBackendLabel) {
    elDbBackendBadge.className = 'meta-chip';
    if (backend === 'MySQL') {
      elDbBackendLabel.textContent = 'MySQL · InnoDB';
    } else if (backend === 'SQLite') {
      elDbBackendBadge.classList.add('sqlite');
      elDbBackendLabel.textContent = 'SQLite · WAL';
    } else {
      elDbBackendBadge.classList.add('disconnected');
      elDbBackendLabel.textContent = `${backend}`;
    }
  }

  if (elPipePid && elPipeHost) {
    const sess = data ? data.active_session : null;
    elPipePid.textContent = (sess && sess.pid) ? `PID ${sess.pid}` : 'PID —';
    elPipeHost.textContent = (sess && sess.hostname) ? sess.hostname : 'localhost';
  }
}

function updateForensicBanner() {
  if (!elForensicBanner) return;
  const rawAlerts = Number(cachedAlertsData.active_count || 0);
  const rawCrashed = Boolean(cachedSessionsData.has_crashed);

  const showAlerts = rawAlerts > 0 && getCurrentAlertKey() !== dismissedAlertKey && activeTab !== 'alerts';
  const showCrash = rawCrashed && getCurrentCrashKey() !== dismissedCrashKey && activeTab !== 'sessions';

  if (!showAlerts && !showCrash) {
    elForensicBanner.style.display = 'none';
    return;
  }

  elForensicBanner.style.display = 'flex';
  const parts = [];
  if (showAlerts) {
    parts.push(`${rawAlerts} active threshold breach${rawAlerts > 1 ? 'es' : ''}`);
  }
  if (showCrash) {
    parts.push('previous daemon session terminated unexpectedly');
  }

  elBannerTitle.textContent = showAlerts ? 'ALERT' : 'CRASH DETECTED';
  elBannerDetail.textContent = parts.join(' · ');
  if (elBannerAlertsBtn) elBannerAlertsBtn.style.display = showAlerts ? 'inline-block' : 'none';
  if (elBannerSessionsBtn) elBannerSessionsBtn.style.display = showCrash ? 'inline-block' : 'none';
}

// updates gauges and status badge from vitals payload
function renderVitals(data) {
  if (!data) return;

  // engine status indicator
  if (data.cpp_engine_active) {
    elEngineBadge.className = 'status-badge';
    const sessionNote = data.session_id ? ` · Session #${data.session_id}` : '';
    elEngineStatus.textContent = `C++ Engine Live${sessionNote}`;
    if (elLivePulse) elLivePulse.style.display = 'block';
  } else {
    elEngineBadge.className = 'status-badge offline';
    if (data.timestamp === 'DB Not Found' || data.timestamp === 'DB Not Connected') {
      elEngineStatus.textContent = 'DB Disconnected';
    } else if (data.age_seconds !== null && data.age_seconds !== undefined && data.age_seconds > 4) {
      elEngineStatus.textContent = `Engine Stopped (${data.age_seconds}s ago)`;
    } else {
      elEngineStatus.textContent = 'Engine Offline';
    }
    if (elLivePulse) elLivePulse.style.display = 'none';
  }

  const cpu = Number(data.cpu_usage) || 0;
  const ram = Number(data.ram_usage) || 0;
  const temp = Number(data.cpu_temp) || 0;
  const fan = Number(data.fan_speed) || 0;

  // cpu card
  elCpuVal.textContent = `${cpu.toFixed(1)}%`;
  elCpuBar.style.width = `${Math.min(100, Math.max(0, cpu))}%`;
  updateDeltaEl(elCpuDelta, cpu, prevVitalsSnapshot ? prevVitalsSnapshot.cpu : null, '%', 1);

  // ram card
  elRamVal.textContent = `${ram.toFixed(1)}%`;
  elRamBar.style.width = `${Math.min(100, Math.max(0, ram))}%`;
  updateDeltaEl(elRamDelta, ram, prevVitalsSnapshot ? prevVitalsSnapshot.ram : null, '%', 1);

  // temp card
  elTempVal.textContent = `${temp.toFixed(1)} °C`;
  elTempBar.style.width = `${Math.min(100, Math.max(0, temp))}%`;
  updateDeltaEl(elTempDelta, temp, prevVitalsSnapshot ? prevVitalsSnapshot.temp : null, '°C', 1);

  // fan card
  elFanVal.textContent = fan > 0 ? `${fan.toLocaleString()} RPM` : '0 RPM';
  elFanBar.style.width = `${Math.min(100, Math.max(0, (fan / 5500) * 100))}%`;
  updateDeltaEl(elFanDelta, fan, prevVitalsSnapshot ? prevVitalsSnapshot.fan : null, ' RPM', 0);

  prevVitalsSnapshot = { cpu, ram, temp, fan };

  if (latestHistoryPoints.length < 2) {
    updateStatsFooter(elCpuStats, [cpu], '%', 1);
    updateStatsFooter(elRamStats, [ram], '%', 1);
    updateStatsFooter(elTempStats, [temp], '°C', 1);
    updateStatsFooter(elFanStats, [fan], '', 0);
  }

  // timestamp text
  if (data.timestamp) {
    elLastUpdated.textContent = data.timestamp;
  }

  updateHeaderChips(data);
}

// marks UI offline when HTTP server cannot be reached
function setServerOffline() {
  elEngineBadge.className = 'status-badge offline';
  elEngineStatus.textContent = 'Server Offline';
  if (elLivePulse) elLivePulse.style.display = 'none';
}

function getFilteredDeletions(items) {
  const q = searchQuery.trim().toLowerCase();
  return items.filter(item => {
    if (fileEventFilter !== 'ALL' && item.event_type !== fileEventFilter) return false;
    if (!q) return true;
    const hay = `${item.id} ${item.timestamp} ${item.event_type} ${item.file_name} ${item.new_name || ''}`.toLowerCase();
    return hay.includes(q);
  });
}

// updates file audit table only when rows actually change
function renderDeletions(data) {
  if (!data) return;
  cachedDeletionsData = data;
  const items = data.items || [];
  const totalCount = data.total_count ?? items.length;
  elDeletionCount.textContent = `${totalCount}`;

  const filtered = getFilteredDeletions(items);
  const sig = items.length === 0
    ? `empty:${fileEventFilter}:${searchQuery}`
    : `${totalCount}:${items.length}:${items[0].id}:${items[items.length - 1].id}:${fileEventFilter}:${searchQuery}`;
  if (sig === lastDeletionsSig) return;
  lastDeletionsSig = sig;

  if (filtered.length === 0) {
    const msg = items.length === 0
      ? 'Watching ./test_watch/ for file deletion &amp; move events...'
      : 'No file events match the active filter.';
    elDeletionsBody.innerHTML = `
      <tr>
        <td colspan="4" class="empty-hint">${msg}</td>
      </tr>
    `;
    return;
  }

  elDeletionsBody.innerHTML = filtered
    .map(item => {
      let badgeClass = 'ev-deleted';
      if (item.event_type === 'MOVED_OUT') badgeClass = 'ev-moved';
      else if (item.event_type === 'RENAMED') badgeClass = 'ev-renamed';
      else if (item.event_type === 'WATCH_DIR_REMOVED' || item.event_type === 'QUEUE_OVERFLOW') badgeClass = 'ev-special';

      const dirTag = item.is_dir ? '<span class="dir-pill">DIR</span>' : '';
      let targetText = `${dirTag}${escapeHtml(item.file_name)}`;
      if (item.new_name) {
        targetText += ` → <span style="color:#c084fc;">${escapeHtml(item.new_name)}</span>`;
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

function getFilteredAlerts(items) {
  const q = searchQuery.trim().toLowerCase();
  if (!q) return items;
  return items.filter(a => {
    const hay = `${a.id} ${a.timestamp} ${a.metric} ${a.value} ${a.threshold} ${a.state}`.toLowerCase();
    return hay.includes(q);
  });
}

// updates alerts table and active count badge only when rows actually change
function renderAlerts(data) {
  if (!data) return;
  cachedAlertsData = data;
  const items = data.items || [];
  const activeCount = data.active_count ?? 0;
  elAlertCount.textContent = `${activeCount}`;
  updateForensicBanner();

  const filtered = getFilteredAlerts(items);
  const sig = items.length === 0
    ? `empty:${activeCount}:${searchQuery}`
    : `${activeCount}:${items.length}:${items[0].id}:${items[0].state}:${items[items.length - 1].id}:${searchQuery}`;
  if (sig === lastAlertsSig) return;
  lastAlertsSig = sig;

  if (filtered.length === 0) {
    const msg = items.length === 0
      ? 'All systems nominal. No threshold breaches recorded.'
      : 'No alerts match the current search filter.';
    elAlertsBody.innerHTML = `
      <tr>
        <td colspan="6" class="empty-hint">${msg}</td>
      </tr>
    `;
    return;
  }

  elAlertsBody.innerHTML = filtered
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

function formatDuration(startStr, endStr) {
  if (!startStr) return '—';
  const startDate = new Date(String(startStr).replace(' ', 'T'));
  if (Number.isNaN(startDate.getTime())) return '—';
  const endDate = endStr ? new Date(String(endStr).replace(' ', 'T')) : new Date();
  if (Number.isNaN(endDate.getTime())) return '—';

  const diffSec = Math.max(0, Math.floor((endDate.getTime() - startDate.getTime()) / 1000));
  if (diffSec < 60) return `${diffSec}s`;
  const mins = Math.floor(diffSec / 60);
  const secs = diffSec % 60;
  if (mins < 60) return `${mins}m ${secs}s`;
  const hrs = Math.floor(mins / 60);
  const remMins = mins % 60;
  if (hrs < 24) return `${hrs}h ${remMins}m`;
  const days = Math.floor(hrs / 24);
  return `${days}d ${hrs % 24}h`;
}

function getFilteredSessions(items) {
  const q = searchQuery.trim().toLowerCase();
  if (!q) return items;
  return items.filter(s => {
    const statusLabel = s.clean_exit === 1 ? 'clean exit' : s.clean_exit === 0 ? 'crashed abrupt termination' : 'active recording';
    const hay = `${s.id} ${s.start_time} ${s.end_time || ''} ${s.pid} ${s.hostname || ''} ${statusLabel}`.toLowerCase();
    return hay.includes(q);
  });
}

// updates session history table only when session state changes
function renderSessions(data) {
  if (!data) return;
  cachedSessionsData = data;
  const items = data.items || [];
  elSessionCount.textContent = `${items.length}`;
  updateForensicBanner();

  const filtered = getFilteredSessions(items);
  const sig = items.length === 0
    ? `empty:${searchQuery}`
    : `${items.map(s => `${s.id}:${s.end_time || ''}:${s.clean_exit ?? 'null'}`).join('|')}:${searchQuery}`;
  if (sig === lastSessionsSig) return;
  lastSessionsSig = sig;

  if (filtered.length === 0) {
    const msg = items.length === 0
      ? 'No session history found.'
      : 'No sessions match the current search filter.';
    elSessionsBody.innerHTML = `
      <tr>
        <td colspan="7" class="empty-hint">${msg}</td>
      </tr>
    `;
    return;
  }

  elSessionsBody.innerHTML = filtered
    .map(s => {
      let statusHtml = '';
      if (s.clean_exit === 1) {
        statusHtml = '<span class="session-clean">Clean Exit</span>';
      } else if (s.clean_exit === 0) {
        statusHtml = '<span class="session-crashed">Crashed / Abrupt Termination</span>';
      } else {
        statusHtml = '<span class="session-active">Active Recording</span>';
      }
      const duration = formatDuration(s.start_time, s.end_time);
      return `
        <tr>
          <td>#${s.id}</td>
          <td>${escapeHtml(s.start_time)}</td>
          <td>${s.end_time ? escapeHtml(s.end_time) : '<span style="color:#38bdf8;">In Progress</span>'}</td>
          <td>${escapeHtml(duration)}</td>
          <td><code>PID ${s.pid}</code></td>
          <td>${escapeHtml(s.hostname || '-')}</td>
          <td>${statusHtml}</td>
        </tr>
      `;
    })
    .join('');
}

// draws interactive multi-metric telemetry timeline chart
function renderMainChart(highlightIdx = -1) {
  if (!elMainChart) return;
  const pts = latestHistoryPoints;
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const cssW = elMainChart.clientWidth || 800;
  const cssH = elMainChart.clientHeight || 165;
  const targetW = Math.round(cssW * dpr);
  const targetH = Math.round(cssH * dpr);

  if (elMainChart.width !== targetW || elMainChart.height !== targetH) {
    elMainChart.width = targetW;
    elMainChart.height = targetH;
  }

  const ctx = elMainChart.getContext('2d');
  ctx.save();
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, cssW, cssH);

  if (!pts || pts.length < 2) {
    if (elMainChartEmpty) elMainChartEmpty.style.display = 'flex';
    mainChartState = null;
    ctx.restore();
    return;
  }

  if (elMainChartEmpty) elMainChartEmpty.style.display = 'none';

  const pad = { top: 14, right: 14, bottom: 24, left: 38 };
  const plotW = Math.max(40, cssW - pad.left - pad.right);
  const plotH = Math.max(40, cssH - pad.top - pad.bottom);

  let fanMax = 5500;
  for (let i = 0; i < pts.length; i++) {
    if (pts[i].fan > fanMax) fanMax = pts[i].fan;
  }

  ctx.font = '500 10px "JetBrains Mono", monospace';
  ctx.textBaseline = 'middle';
  ctx.textAlign = 'right';

  const gridLevels = [0, 25, 50, 75, 100];
  for (const pct of gridLevels) {
    const y = pad.top + plotH - (pct / 100) * plotH;
    ctx.strokeStyle = pct === 0 ? 'rgba(255, 255, 255, 0.1)' : 'rgba(255, 255, 255, 0.045)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(pad.left, y);
    ctx.lineTo(pad.left + plotW, y);
    ctx.stroke();

    ctx.fillStyle = '#64748b';
    const axisLabel = activeChartSeries === 'fan'
      ? `${Math.round((pct / 100) * fanMax)}`
      : activeChartSeries === 'temp'
        ? `${pct}°`
        : `${pct}%`;
    ctx.fillText(axisLabel, pad.left - 6, y);
  }

  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  ctx.fillStyle = '#64748b';
  const tickCount = Math.min(6, pts.length);
  for (let i = 0; i < tickCount; i++) {
    const idx = tickCount === 1 ? 0 : Math.round((i / (tickCount - 1)) * (pts.length - 1));
    const x = pad.left + (idx / (pts.length - 1)) * plotW;
    if (i === 0) ctx.textAlign = 'left';
    else if (i === tickCount - 1) ctx.textAlign = 'right';
    else ctx.textAlign = 'center';
    ctx.fillText(pts[idx].t, x, pad.top + plotH + 6);
  }

  const seriesMeta = [
    { key: 'cpu', color: '#38bdf8', fill: 'rgba(56, 189, 248, 0.14)', max: 100 },
    { key: 'ram', color: '#10b981', fill: 'rgba(16, 185, 129, 0.14)', max: 100 },
    { key: 'temp', color: '#f59e0b', fill: 'rgba(245, 158, 11, 0.14)', max: 100 },
    { key: 'fan', color: '#a855f7', fill: 'rgba(168, 85, 247, 0.14)', max: fanMax },
  ];

  const visibleSeries = activeChartSeries === 'all'
    ? seriesMeta
    : seriesMeta.filter(s => s.key === activeChartSeries);

  const stepX = plotW / (pts.length - 1);
  const getX = idx => pad.left + idx * stepX;
  const getY = (val, maxVal) => {
    const clamped = Math.max(0, Math.min(maxVal, Number(val) || 0));
    return pad.top + plotH - (clamped / maxVal) * plotH;
  };

  for (const s of visibleSeries) {
    if (visibleSeries.length === 1 || s.key === 'cpu') {
      const grad = ctx.createLinearGradient(0, pad.top, 0, pad.top + plotH);
      grad.addColorStop(0, s.fill);
      grad.addColorStop(1, 'rgba(0, 0, 0, 0)');
      ctx.beginPath();
      ctx.moveTo(getX(0), getY(pts[0][s.key], s.max));
      for (let i = 1; i < pts.length; i++) {
        ctx.lineTo(getX(i), getY(pts[i][s.key], s.max));
      }
      ctx.lineTo(getX(pts.length - 1), pad.top + plotH);
      ctx.lineTo(getX(0), pad.top + plotH);
      ctx.closePath();
      ctx.fillStyle = grad;
      ctx.fill();
    }

    ctx.beginPath();
    ctx.moveTo(getX(0), getY(pts[0][s.key], s.max));
    for (let i = 1; i < pts.length; i++) {
      ctx.lineTo(getX(i), getY(pts[i][s.key], s.max));
    }
    ctx.strokeStyle = s.color;
    ctx.lineWidth = visibleSeries.length === 1 ? 2.1 : 1.7;
    ctx.lineJoin = 'round';
    ctx.lineCap = 'round';
    ctx.stroke();
  }

  if (highlightIdx >= 0 && highlightIdx < pts.length) {
    const hx = getX(highlightIdx);
    ctx.strokeStyle = 'rgba(248, 250, 252, 0.25)';
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 3]);
    ctx.beginPath();
    ctx.moveTo(hx, pad.top);
    ctx.lineTo(hx, pad.top + plotH);
    ctx.stroke();
    ctx.setLineDash([]);

    for (const s of visibleSeries) {
      const hy = getY(pts[highlightIdx][s.key], s.max);
      ctx.beginPath();
      ctx.arc(hx, hy, 3.5, 0, Math.PI * 2);
      ctx.fillStyle = s.color;
      ctx.fill();
      ctx.strokeStyle = '#090d17';
      ctx.lineWidth = 1.5;
      ctx.stroke();
    }
  }

  mainChartState = { pad, plotW, plotH, fanMax, cssW, cssH };
  ctx.restore();
}

// repaints sparkline charts only when time-series points advance
function renderHistory(data) {
  if (!data) return;
  const pts = data.points || [];
  if (elHistoryPointCount) {
    elHistoryPointCount.textContent = `${pts.length} pts`;
  }

  const last = pts.length > 0 ? pts[pts.length - 1] : null;
  const sig = last
    ? `${pts.length}:${pts[0].t}:${last.t}:${last.cpu}:${last.ram}:${last.temp}:${last.fan}:${historyMinutes}`
    : `empty:${historyMinutes}`;
  if (sig === lastHistorySig) return;
  lastHistorySig = sig;
  latestHistoryPoints = pts;

  if (pts.length < 2) {
    drawSparkline('cpuSpark', null, 100, '#38bdf8', 'rgba(56, 189, 248, 0.2)', 90);
    drawSparkline('ramSpark', null, 100, '#10b981', 'rgba(16, 185, 129, 0.2)', 90);
    drawSparkline('tempSpark', null, 100, '#f59e0b', 'rgba(245, 158, 11, 0.2)', 85);
    drawSparkline('fanSpark', null, 5500, '#a855f7', 'rgba(168, 85, 247, 0.2)');
    renderMainChart(-1);
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

  drawSparkline('cpuSpark', cpus, 100, '#38bdf8', 'rgba(56, 189, 248, 0.2)', 90);
  drawSparkline('ramSpark', rams, 100, '#10b981', 'rgba(16, 185, 129, 0.2)', 90);
  drawSparkline('tempSpark', temps, 100, '#f59e0b', 'rgba(245, 158, 11, 0.2)', 85);
  drawSparkline('fanSpark', fans, fanMax, '#a855f7', 'rgba(168, 85, 247, 0.2)');

  updateStatsFooter(elCpuStats, cpus, '%', 1);
  updateStatsFooter(elRamStats, rams, '%', 1);
  updateStatsFooter(elTempStats, temps, '°C', 1);
  updateStatsFooter(elFanStats, fans, '', 0);

  renderMainChart(hoverPointIdx);
}

// exports currently active forensic tab rows as CSV download
function exportActiveTabCsv() {
  let headers = [];
  let rows = [];
  const filename = `blackbox_${activeTab}.csv`;

  if (activeTab === 'files') {
    headers = ['ID', 'Timestamp', 'Event', 'IsDirectory', 'FileName', 'NewName', 'SessionID'];
    rows = getFilteredDeletions(cachedDeletionsData.items || []).map(item => [
      item.id,
      item.timestamp,
      item.event_type,
      item.is_dir ? 'true' : 'false',
      item.file_name,
      item.new_name || '',
      item.session_id ?? '',
    ]);
  } else if (activeTab === 'alerts') {
    headers = ['ID', 'Timestamp', 'Metric', 'Value', 'Threshold', 'State', 'SessionID'];
    rows = getFilteredAlerts(cachedAlertsData.items || []).map(a => [
      a.id,
      a.timestamp,
      a.metric,
      a.value,
      a.threshold,
      a.state,
      a.session_id ?? '',
    ]);
  } else {
    headers = ['SessionID', 'StartTime', 'EndTime', 'Duration', 'PID', 'Hostname', 'Status'];
    rows = getFilteredSessions(cachedSessionsData.items || []).map(s => [
      s.id,
      s.start_time,
      s.end_time || 'In Progress',
      formatDuration(s.start_time, s.end_time),
      s.pid,
      s.hostname || '',
      s.clean_exit === 1 ? 'Clean Exit' : s.clean_exit === 0 ? 'Crashed' : 'Active Recording',
    ]);
  }

  const escapeCsv = val => {
    const str = String(val ?? '');
    if (/[",\n\r]/.test(str)) {
      return `"${str.replace(/"/g, '""')}"`;
    }
    return str;
  };

  const csvLines = [
    headers.map(escapeCsv).join(','),
    ...rows.map(r => r.map(escapeCsv).join(',')),
  ];
  const blob = new Blob([csvLines.join('\n')], { type: 'text/csv;charset=utf-8;' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  URL.revokeObjectURL(url);
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
    const res = await fetch(`/api/history?minutes=${historyMinutes}`, { cache: 'no-store' });
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

// interactive chart hover inspection
if (elMainChart) {
  elMainChart.addEventListener('mousemove', evt => {
    if (!mainChartState || latestHistoryPoints.length < 2) return;
    const rect = elMainChart.getBoundingClientRect();
    const x = evt.clientX - rect.left;
    const { pad, plotW, cssW } = mainChartState;

    const rel = Math.max(0, Math.min(1, (x - pad.left) / plotW));
    const idx = Math.round(rel * (latestHistoryPoints.length - 1));
    hoverPointIdx = idx;
    renderMainChart(idx);

    const pt = latestHistoryPoints[idx];
    if (elChartTooltip && pt) {
      elChartTooltip.innerHTML = `
        <div class="tt-time">${escapeHtml(pt.t)}</div>
        <div class="tt-row"><span class="tt-key"><span class="dot dot-cpu"></span>CPU</span><span>${Number(pt.cpu).toFixed(1)}%</span></div>
        <div class="tt-row"><span class="tt-key"><span class="dot dot-ram"></span>RAM</span><span>${Number(pt.ram).toFixed(1)}%</span></div>
        <div class="tt-row"><span class="tt-key"><span class="dot dot-temp"></span>Temp</span><span>${Number(pt.temp).toFixed(1)} °C</span></div>
        <div class="tt-row"><span class="tt-key"><span class="dot dot-fan"></span>Fan</span><span>${Number(pt.fan).toLocaleString()} RPM</span></div>
      `;
      elChartTooltip.style.display = 'block';
      const ttW = elChartTooltip.offsetWidth || 160;
      const pointX = pad.left + rel * plotW;
      const leftPos = (pointX + ttW + 16 > cssW) ? Math.max(8, pointX - ttW - 12) : pointX + 12;
      elChartTooltip.style.left = `${leftPos}px`;
      elChartTooltip.style.top = '10px';
    }
  });

  elMainChart.addEventListener('mouseleave', () => {
    hoverPointIdx = -1;
    if (elChartTooltip) elChartTooltip.style.display = 'none';
    renderMainChart(-1);
  });
}

// instant client-side search across active forensic tables
if (elAuditSearchInput) {
  elAuditSearchInput.addEventListener('input', evt => {
    searchQuery = evt.target.value || '';
    lastDeletionsSig = null;
    lastAlertsSig = null;
    lastSessionsSig = null;
    renderDeletions(cachedDeletionsData);
    renderAlerts(cachedAlertsData);
    renderSessions(cachedSessionsData);
  });
}

// responsive canvas redraw on window resize
window.addEventListener('resize', () => {
  lastHistorySig = null;
  renderHistory({ points: latestHistoryPoints });
});

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
    const url = full ? `/api/snapshot?full=1&minutes=${historyMinutes}` : '/api/snapshot?full=0';
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
