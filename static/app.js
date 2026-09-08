/**
 * app.js — System Blackbox Web Telemetry Client
 * Reads exclusively from /api/metrics and /api/deletions (backed by SQLite).
 */

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

async function updateVitals() {
  try {
    const res = await fetch('/api/metrics', { cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();

    // 1. Engine Status & DB Connectivity
    if (data.cpp_engine_active) {
      elEngineBadge.className = 'status-badge';
      elEngineStatus.textContent = 'C++ Daemon Live (SQLite WAL)';
      elLivePulse.style.display = 'block';
    } else {
      elEngineBadge.className = 'status-badge offline';
      elEngineStatus.textContent = data.timestamp === 'DB Not Found' ? 'DB Not Found' : 'Engine Offline';
      elLivePulse.style.display = 'none';
    }

    // 2. CPU Usage
    elCpuVal.textContent = `${Number(data.cpu_usage).toFixed(1)}%`;
    elCpuBar.style.width = `${Math.min(100, Math.max(0, data.cpu_usage))}%`;

    // 3. RAM Usage
    elRamVal.textContent = `${Number(data.ram_usage).toFixed(1)}%`;
    elRamBar.style.width = `${Math.min(100, Math.max(0, data.ram_usage))}%`;

    // 4. CPU Temp
    elTempVal.textContent = `${Number(data.cpu_temp).toFixed(1)} °C`;
    elTempBar.style.width = `${Math.min(100, Math.max(0, (data.cpu_temp / 100) * 100))}%`;

    // 5. Fan Speed
    const fan = Number(data.fan_speed) || 0;
    elFanVal.textContent = fan > 0 ? `${fan.toLocaleString()} RPM` : '0 RPM';
    elFanBar.style.width = `${Math.min(100, Math.max(0, (fan / 5500) * 100))}%`;

    // 6. Timestamp
    if (data.timestamp) {
      elLastUpdated.textContent = data.timestamp;
    }
  } catch (err) {
    elEngineBadge.className = 'status-badge offline';
    elEngineStatus.textContent = 'Server Offline';
    elLivePulse.style.display = 'none';
  }
}

async function updateDeletions() {
  try {
    const res = await fetch('/api/deletions', { cache: 'no-store' });
    if (!res.ok) return;
    const items = await res.json();

    elDeletionCount.textContent = `${items.length} events`;

    if (!items || items.length === 0) {
      elDeletionsBody.innerHTML = `
        <tr>
          <td colspan="3" class="empty-hint">Watching ./test_watch/ for file deletion events...</td>
        </tr>
      `;
      return;
    }

    elDeletionsBody.innerHTML = items
      .map(
        (item) => `
        <tr>
          <td>#${item.id}</td>
          <td>${item.timestamp}</td>
          <td><span class="file-target">🗑 ${escapeHtml(item.file_name)}</span></td>
        </tr>
      `
      )
      .join('');
  } catch (err) {
    console.error('Failed to load deletions audit log:', err);
  }
}

function escapeHtml(text) {
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

// Initial fetch and intervals
updateVitals();
updateDeletions();

setInterval(updateVitals, 1000);
setInterval(updateDeletions, 2000);
