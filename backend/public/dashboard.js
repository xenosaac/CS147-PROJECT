// Dashboard for the Construction Site Worker Safety Monitor.
// Plain JS, no build step. Polls /api/latest every 5s and /api/history every
// 30s. Uses Chart.js (loaded via CDN <script> in index.html).

(function () {
  'use strict';

  const LATEST_POLL_MS = 5000;
  const HISTORY_POLL_MS = 30000;
  const OFFLINE_THRESHOLD_SEC = 30;
  const DEFAULT_RANGE_MIN = 5;

  // Selected chart time range, in minutes. Changed by the range buttons;
  // pollHistory() always fetches whatever this currently is.
  let currentRangeMin = DEFAULT_RANGE_MIN;

  const ALERT_LABELS = {
    normal: 'NORMAL',
    high_heat: 'HIGH HEAT',
    reminder_due: 'REMINDER DUE',
    escalated: 'ESCALATED',
  };

  const el = {
    badge: document.getElementById('status-badge'),
    temp: document.getElementById('temp-value'),
    humidity: document.getElementById('humidity-value'),
    heatIndex: document.getElementById('heat-index-value'),
    lastCheckin: document.getElementById('last-checkin'),
    lastUpdate: document.getElementById('last-update'),
    deviceId: document.getElementById('device-id'),
    lastUpdateRow: document.getElementById('last-update').closest('.meta-row'),
  };

  // Tracks the most recent successful /api/latest fetch so a 1s local ticker
  // can smoothly age "Ns ago" labels between the 5s network polls.
  let lastFetch = null; // { reading, receivedAt, staleSeconds, fetchedAtClientMs }

  let chart = null;

  function fmtNumber(value, unit, digits) {
    if (typeof value !== 'number' || !Number.isFinite(value)) return '--';
    return value.toFixed(digits === undefined ? 1 : digits) + unit;
  }

  // API/firmware contract is Celsius throughout; convert to Fahrenheit only
  // here, at display time, for a US audience.
  function cToF(celsius) {
    if (typeof celsius !== 'number' || !Number.isFinite(celsius)) return celsius;
    return (celsius * 9) / 5 + 32;
  }

  function setBadge(alertLevel, offline) {
    el.badge.className = 'badge';
    if (offline || !alertLevel) {
      el.badge.classList.add('badge-offline');
      el.badge.textContent = offline ? 'OFFLINE' : 'UNKNOWN';
      return;
    }
    el.badge.classList.add('badge-' + alertLevel);
    el.badge.textContent = ALERT_LABELS[alertLevel] || alertLevel.toUpperCase();
  }

  async function pollLatest() {
    try {
      const res = await fetch('/api/latest');
      const data = await res.json();
      if (!data.ok) return;

      if (!data.reading) {
        lastFetch = null;
        setBadge(null, false);
        el.temp.textContent = '--';
        el.humidity.textContent = '--';
        el.heatIndex.textContent = '--';
        el.deviceId.textContent = '--';
        el.lastCheckin.textContent = '--';
        el.lastUpdate.textContent = '--';
        return;
      }

      lastFetch = {
        reading: data.reading,
        receivedAt: data.receivedAt,
        staleSeconds: data.staleSeconds,
        fetchedAtClientMs: Date.now(),
      };

      el.temp.textContent = fmtNumber(cToF(data.reading.tempC), ' °F');
      el.humidity.textContent = fmtNumber(data.reading.humidityPct, ' %');
      el.heatIndex.textContent = fmtNumber(cToF(data.reading.heatIndexC), ' °F');
      el.deviceId.textContent = data.reading.deviceId || '--';

      renderAgeLabels();
    } catch (err) {
      // Network/server hiccup: leave last-known display in place, ticker
      // will keep aging it and eventually show OFFLINE via staleSeconds.
      console.warn('pollLatest failed', err);
    }
  }

  function renderAgeLabels() {
    if (!lastFetch) {
      setBadge(null, false);
      return;
    }

    const elapsedSinceFetchSec = Math.floor((Date.now() - lastFetch.fetchedAtClientMs) / 1000);
    const liveStaleSeconds =
      typeof lastFetch.staleSeconds === 'number' ? lastFetch.staleSeconds + elapsedSinceFetchSec : null;
    const liveCheckinSeconds =
      typeof lastFetch.reading.secondsSinceLastCheckin === 'number'
        ? lastFetch.reading.secondsSinceLastCheckin + elapsedSinceFetchSec
        : null;

    const offline = liveStaleSeconds !== null && liveStaleSeconds > OFFLINE_THRESHOLD_SEC;

    el.lastUpdate.textContent = liveStaleSeconds !== null ? liveStaleSeconds + 's ago' : '--';
    el.lastCheckin.textContent = liveCheckinSeconds !== null ? liveCheckinSeconds + 's ago' : '--';

    if (el.lastUpdateRow) {
      el.lastUpdateRow.classList.toggle('offline', offline);
    }

    setBadge(lastFetch.reading.alertLevel, offline);
  }

  // x values are real timestamps on a linear scale (not category labels), so
  // gaps in data show as blank space instead of being compressed away.
  function initChart() {
    const ctx = document.getElementById('history-chart');
    chart = new Chart(ctx, {
      type: 'line',
      data: {
        datasets: [
          {
            label: 'Temp (°F)',
            data: [],
            borderColor: '#3498db',
            backgroundColor: 'transparent',
            tension: 0.25,
            pointRadius: 0,
          },
          {
            label: 'Heat Index (°F)',
            data: [],
            borderColor: '#e67e22',
            backgroundColor: 'transparent',
            tension: 0.25,
            pointRadius: 0,
          },
        ],
      },
      options: {
        responsive: true,
        animation: false,
        scales: {
          x: {
            type: 'linear',
            min: Date.now() - currentRangeMin * 60000,
            max: Date.now(),
            ticks: {
              color: '#8a90a2',
              maxTicksLimit: 8,
              callback: (value) => timeLabel(value),
            },
            grid: { color: '#2a2e3a' },
          },
          y: {
            ticks: { color: '#8a90a2' },
            grid: { color: '#2a2e3a' },
          },
        },
        plugins: {
          legend: { labels: { color: '#e8eaf0' } },
        },
      },
    });
  }

  // Short ranges show seconds (confirms the feed is live); longer ranges are
  // trend charts, so minute/date resolution reads better than noisy seconds.
  function timeLabel(receivedAtMs) {
    if (!receivedAtMs) return '';
    const d = new Date(receivedAtMs);
    if (currentRangeMin <= 5) {
      return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    }
    if (currentRangeMin <= 180) {
      return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
    }
    return d.toLocaleString([], { month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit' });
  }

  async function pollHistory() {
    try {
      const res = await fetch('/api/history?rangeMin=' + currentRangeMin);
      const data = await res.json();
      if (!data.ok || !Array.isArray(data.readings)) return;

      const tempSeries = data.readings.map((r) => ({ x: r.receivedAt, y: cToF(r.tempC) }));
      const heatIndexSeries = data.readings.map((r) => ({ x: r.receivedAt, y: cToF(r.heatIndexC) }));

      if (!chart) return;
      const now = Date.now();
      chart.options.scales.x.min = now - currentRangeMin * 60000;
      chart.options.scales.x.max = now;
      chart.data.datasets[0].data = tempSeries;
      chart.data.datasets[1].data = heatIndexSeries;
      chart.update();
    } catch (err) {
      console.warn('pollHistory failed', err);
    }
  }

  function initRangeButtons() {
    const container = document.getElementById('range-select');
    if (!container) return;
    const buttons = Array.from(container.querySelectorAll('.range-btn'));

    function setActive(rangeMin) {
      currentRangeMin = rangeMin;
      buttons.forEach((btn) => {
        btn.classList.toggle('active', Number(btn.dataset.rangeMin) === rangeMin);
      });
    }

    buttons.forEach((btn) => {
      btn.addEventListener('click', () => {
        const rangeMin = Number(btn.dataset.rangeMin);
        if (rangeMin === currentRangeMin) return;
        setActive(rangeMin);
        pollHistory();
      });
    });

    setActive(DEFAULT_RANGE_MIN);
  }

  initChart();
  initRangeButtons();
  pollLatest();
  pollHistory();

  setInterval(pollLatest, LATEST_POLL_MS);
  setInterval(pollHistory, HISTORY_POLL_MS);
  setInterval(renderAgeLabels, 1000);
})();
