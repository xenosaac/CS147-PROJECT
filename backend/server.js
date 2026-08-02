'use strict';

/**
 * Backend for the Construction Site Worker Safety Monitor (CS147 project).
 */

require('dotenv').config();

const express = require('express');
const fs = require('fs');
const path = require('path');

const PORT = process.env.PORT || 3147;
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || '';

const DATA_DIR = path.join(__dirname, 'data');
const READINGS_FILE = path.join(DATA_DIR, 'readings.jsonl');
const PUBLIC_DIR = path.join(__dirname, 'public');

const ALERT_LEVELS = ['normal', 'high_heat', 'reminder_due', 'escalated'];

// In-memory cache of the most recent reading; seeded from readings.jsonl on startup.
let latestReading = null; // { ...reading fields..., receivedAt } | null

function ensureDataFile() {
  if (!fs.existsSync(DATA_DIR)) {
    fs.mkdirSync(DATA_DIR, { recursive: true });
  }
  if (!fs.existsSync(READINGS_FILE)) {
    fs.writeFileSync(READINGS_FILE, '');
  }
}

function seedLatestFromFile() {
  try {
    const contents = fs.readFileSync(READINGS_FILE, 'utf8');
    const lines = contents.split('\n').filter((line) => line.trim().length > 0);
    if (lines.length === 0) return;
    const lastLine = lines[lines.length - 1];
    latestReading = JSON.parse(lastLine);
  } catch (err) {
    // If the file is missing, empty, or the last line is corrupt, just start
    // with an empty cache rather than crashing the server on boot.
    console.warn('Could not seed latestReading from', READINGS_FILE, '-', err.message);
  }
}

function appendReading(record) {
  fs.appendFileSync(READINGS_FILE, JSON.stringify(record) + '\n');
}

function readLastNReadings(limit) {
  let contents;
  try {
    contents = fs.readFileSync(READINGS_FILE, 'utf8');
  } catch (err) {
    return [];
  }
  const lines = contents.split('\n').filter((line) => line.trim().length > 0);
  const tail = lines.slice(Math.max(0, lines.length - limit));
  const readings = [];
  for (const line of tail) {
    try {
      readings.push(JSON.parse(line));
    } catch (err) {
      // Skip corrupt lines rather than failing the whole request.
      console.warn('Skipping corrupt JSONL line in', READINGS_FILE);
    }
  }
  return readings;
}

function readAllReadings() {
  let contents;
  try {
    contents = fs.readFileSync(READINGS_FILE, 'utf8');
  } catch (err) {
    return [];
  }
  const readings = [];
  for (const line of contents.split('\n')) {
    if (line.trim().length === 0) continue;
    try {
      readings.push(JSON.parse(line));
    } catch (err) {
      console.warn('Skipping corrupt JSONL line in', READINGS_FILE);
    }
  }
  return readings;
}

// A 24h window can be tens of thousands of raw rows — too many to plot
// usefully, so average down into maxPoints buckets instead of truncating.
function bucketReadings(readings, maxPoints) {
  if (readings.length <= maxPoints) return readings;

  const bucketSize = readings.length / maxPoints;
  const buckets = [];
  for (let b = 0; b < maxPoints; b++) {
    const start = Math.floor(b * bucketSize);
    const end = Math.max(start + 1, Math.floor((b + 1) * bucketSize));
    const slice = readings.slice(start, end);
    if (slice.length === 0) continue;

    const avg = (key) =>
      slice.reduce((sum, r) => sum + (typeof r[key] === 'number' ? r[key] : 0), 0) / slice.length;
    const last = slice[slice.length - 1];

    buckets.push({
      deviceId: last.deviceId,
      uptimeMs: last.uptimeMs,
      tempC: avg('tempC'),
      humidityPct: avg('humidityPct'),
      heatIndexC: avg('heatIndexC'),
      alertLevel: last.alertLevel,
      checkinPressed: slice.some((r) => r.checkinPressed === true),
      secondsSinceLastCheckin: last.secondsSinceLastCheckin,
      reminderIntervalSec: last.reminderIntervalSec,
      receivedAt: Math.round(avg('receivedAt')),
    });
  }
  return buckets;
}

// -----------------------------------------------------------------------
// Payload validation for POST /api/readings
// -----------------------------------------------------------------------
function isFiniteNumber(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function validateReadingPayload(body) {
  const details = [];

  if (body === null || typeof body !== 'object' || Array.isArray(body)) {
    return ['body must be a JSON object'];
  }

  if (typeof body.deviceId !== 'string' || body.deviceId.trim().length === 0) {
    details.push('deviceId must be a non-empty string');
  }

  if (!isFiniteNumber(body.uptimeMs)) {
    details.push('uptimeMs must be a finite number');
  }

  if (!isFiniteNumber(body.tempC)) {
    details.push('tempC must be a finite number');
  }

  if (!isFiniteNumber(body.humidityPct)) {
    details.push('humidityPct must be a finite number');
  }

  if (!isFiniteNumber(body.heatIndexC)) {
    details.push('heatIndexC must be a finite number');
  }

  if (typeof body.alertLevel !== 'string' || !ALERT_LEVELS.includes(body.alertLevel)) {
    details.push(`alertLevel must be one of: ${ALERT_LEVELS.join(', ')}`);
  }

  if (typeof body.checkinPressed !== 'boolean') {
    details.push('checkinPressed must be a boolean');
  }

  if (!isFiniteNumber(body.secondsSinceLastCheckin)) {
    details.push('secondsSinceLastCheckin must be a finite number');
  }

  if (!isFiniteNumber(body.reminderIntervalSec)) {
    details.push('reminderIntervalSec must be a finite number');
  }

  return details;
}

// -----------------------------------------------------------------------
// App setup
// -----------------------------------------------------------------------
ensureDataFile();
seedLatestFromFile();

const app = express();
app.use(express.json());

// Catch malformed JSON bodies from express.json() and respond in the same
// shape as other validation failures instead of Express's default HTML error.
app.use((err, req, res, next) => {
  if (err && err.type === 'entity.parse.failed') {
    return res.status(400).json({
      ok: false,
      error: 'invalid payload',
      details: ['body must be valid JSON'],
    });
  }
  return next(err);
});

function requireDeviceAuth(req, res, next) {
  const header = req.get('Authorization') || '';
  const match = header.match(/^Bearer\s+(.+)$/);
  const token = match ? match[1] : null;

  if (!token || token !== DEVICE_TOKEN) {
    return res.status(401).json({ ok: false, error: 'unauthorized' });
  }
  return next();
}

// -----------------------------------------------------------------------
// POST /api/readings — device -> server, the only write endpoint
// -----------------------------------------------------------------------
app.post('/api/readings', requireDeviceAuth, (req, res) => {
  const details = validateReadingPayload(req.body);
  if (details.length > 0) {
    return res.status(400).json({ ok: false, error: 'invalid payload', details });
  }

  const receivedAt = Date.now();
  const record = { ...req.body, receivedAt };

  appendReading(record);
  latestReading = record;

  // Optional Base Sepolia ledger hook — only required when
  // ENABLE_BLOCKCHAIN=true; fire-and-forget, ledger.js
  // catches its own errors so this can't block the HTTP response.
  if (process.env.ENABLE_BLOCKCHAIN === 'true') {
    try {
      const logReadingToChain = require('./ledger');
      logReadingToChain(record).catch((err) => {
        console.error('[ledger] unexpected rejection from logReading:', err);
      });
    } catch (err) {
      console.error('[ledger] failed to load ledger module:', err && err.message ? err.message : err);
    }
  }

  return res.status(201).json({ ok: true, receivedAt });
});

// -----------------------------------------------------------------------
// GET /api/latest — public, no auth
// -----------------------------------------------------------------------
app.get('/api/latest', (req, res) => {
  if (!latestReading) {
    return res.json({ ok: true, reading: null, receivedAt: null, staleSeconds: null });
  }

  const { receivedAt, ...reading } = latestReading;
  const staleSeconds = Math.max(0, Math.floor((Date.now() - receivedAt) / 1000));

  return res.json({ ok: true, reading, receivedAt, staleSeconds });
});

// GET /api/history?limit=200 or ?rangeMin=180 — public, no auth; rangeMin takes precedence when both are given.
app.get('/api/history', (req, res) => {
  const DEFAULT_LIMIT = 200;
  const MAX_LIMIT = 1000;
  const MAX_BUCKETED_POINTS = 200;

  const rangeMin = parseInt(req.query.rangeMin, 10);
  if (Number.isFinite(rangeMin) && rangeMin > 0) {
    const cutoff = Date.now() - rangeMin * 60000;
    const inRange = readAllReadings().filter(
      (r) => typeof r.receivedAt === 'number' && r.receivedAt >= cutoff
    );
    const readings = bucketReadings(inRange, MAX_BUCKETED_POINTS);
    return res.json({ ok: true, readings });
  }

  let limit = parseInt(req.query.limit, 10);
  if (!Number.isFinite(limit) || limit <= 0) {
    limit = DEFAULT_LIMIT;
  }
  limit = Math.min(limit, MAX_LIMIT);

  const readings = readLastNReadings(limit);
  return res.json({ ok: true, readings });
});

// -----------------------------------------------------------------------
// GET /api/health — public, no auth
// -----------------------------------------------------------------------
app.get('/api/health', (req, res) => {
  return res.json({ ok: true, uptimeSec: Math.floor(process.uptime()) });
});

// -----------------------------------------------------------------------
// GET / — serves the static dashboard from public/
// -----------------------------------------------------------------------
app.use(express.static(PUBLIC_DIR));

app.listen(PORT, () => {
  console.log(`Heat safety backend listening on port ${PORT}`);
});
