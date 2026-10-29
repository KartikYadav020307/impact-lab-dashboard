export const COLORS = ['#d3fb86', '#73cfe0', '#eeae7d', '#b5a0e9'];
const TYPES = new Set(['status', 'ready', 'not_ready', 'hit', 'calibrating', 'calibration_warning', 'calibration_error', 'config_error']);
export const bounded = (value, min = 0, max = 4095) => typeof value === 'number' && Number.isFinite(value) && value >= min && value <= max ? value : null;
export function numbers(value) { return Array.from({length: 4}, (_, i) => bounded(Array.isArray(value) ? value[i] : null)); }
export function parseLine(line) {
  const text = line.replace(/&#x20;|&#32;|&nbsp;/g, ' ').trim();
  if (!text.startsWith('{') || text.length > 16384) return null;
  try {
    const value = JSON.parse(text);
    if (!value || typeof value !== 'object' || !TYPES.has(value.type)) return null;
    if (value.type === 'hit' && (!Number.isInteger(value.zone) || value.zone < 0 || value.zone > 4 || bounded(value.peak) === null)) return null;
    return value;
  } catch { return null; }
}
export class LineBuffer {
  constructor(onLine) { this.pending = ''; this.onLine = onLine; this.dropping = false; }
  push(chunk) {
    for (const part of chunk.split(/(?<=\n)/)) {
      if (!part) continue;
      if (!this.dropping) this.pending += part;
      if (this.pending.length > 16384) { this.pending = ''; this.dropping = true; }
      if (part.endsWith('\n')) {
        if (!this.dropping) this.onLine(this.pending);
        this.pending = ''; this.dropping = false;
      }
    }
  }
  flush() { if (this.pending && !this.dropping) this.onLine(this.pending); this.pending = ''; this.dropping = false; }
}
export class Monitor {
  constructor() { this.reset(); }
  reset() {
    this.readings = [null,null,null,null]; this.baseline = [null,null,null,null]; this.thresholds = [null,null,null,null];
    this.min = [null,null,null,null]; this.max = [null,null,null,null]; this.pins = [32,33,34,35]; this.active = 4;
    this.armed = false; this.enabled = false; this.calibration = null; this.firmware = ''; this.mode = '';
    this.events = []; this.total = 0; this.strongest = 0; this.history = []; this.lastSeen = 0; this.lastStatus = 0;
    this.warning = ''; this.messageType = ''; this.uptime = null; this.restarts = 0; this.deviceEvents = null; this.invalid = 0;
  }
  accept(m, now = Date.now()) {
    this.lastSeen = now; this.messageType = m.type;
    if (m.type === 'calibration_warning' || m.type === 'calibration_error') {
      this.warning = m.type === 'calibration_error' ? 'Input calibration failed.' : 'Idle noise is high.';
      if (m.type === 'calibration_error') { this.calibration = false; this.armed = this.enabled = false; }
      return;
    }
    if (m.type === 'config_error') { this.warning = 'Firmware configuration needs attention.'; this.enabled = this.armed = false; return; }
    if (m.type === 'calibrating') { this.enabled = this.armed = false; this.warning = 'Measuring the startup baseline…'; return; }
    if (m.type === 'hit') {
      const candidate = Number.isInteger(m.candidate_zone) && m.candidate_zone >= 1 && m.candidate_zone <= 4 ? m.candidate_zone : null;
      const hit = {zone:m.zone, candidate, peak:m.peak, intensity:bounded(m.intensity,0,1) ?? m.peak/4095, ambiguous:m.ambiguous === true || m.zone === 0, clipped:m.clipped === true, time:now, timeUs:bounded(m.time_us,0,4294967295), peaks:numbers(m.peaks), readings:numbers(m.readings)};
      this.events.unshift(hit); this.events.length = Math.min(this.events.length, 1000);
      this.total++; this.strongest = Math.max(this.strongest, m.peak);
      return hit;
    }
    if (['status','ready','not_ready'].includes(m.type)) {
      this.lastStatus = now;
      const uptime = bounded(m.uptime_ms,0,4294967295);
      if (uptime !== null && this.uptime !== null && uptime < this.uptime) { this.restarts++; this.history = []; }
      this.uptime = uptime;
      this.readings = numbers(m.readings); this.baseline = numbers(m.baseline); this.thresholds = numbers(m.thresholds);
      this.min = numbers(m.window_min); this.max = numbers(m.window_max);
      this.active = Number.isInteger(m.active_sensors) && m.active_sensors >= 1 && m.active_sensors <= 4 ? m.active_sensors : 4;
      if (Array.isArray(m.pins)) this.pins = Array.from({length:4},(_,i)=>bounded(m.pins[i],0,48));
      for (let i=this.active;i<4;i++) this.readings[i] = this.baseline[i] = this.thresholds[i] = this.min[i] = this.max[i] = null;
      this.armed = m.armed === true;
      this.calibration = typeof m.calibration_ok === 'boolean' ? m.calibration_ok : null;
      this.enabled = typeof m.detection_enabled === 'boolean' ? m.detection_enabled : this.calibration !== false && m.type !== 'not_ready';
      if (!this.enabled) this.armed = false;
      this.firmware = typeof m.firmware === 'string' ? m.firmware.slice(0,80) : '';
      this.mode = typeof m.mode === 'string' ? m.mode.slice(0,80) : 'auto';
      this.deviceEvents = bounded(m.events,0,4294967295);
      this.warning = this.calibration === false ? (this.enabled ? 'Idle noise is high; large-spike detection is enabled.' : 'Calibration failed; detection is not enabled.') : '';
      this.history.push({time:now,values:[...this.readings]}); if (this.history.length>60) this.history.shift();
    }
  }
}
export function exportCSV(events) {
  const rows = [['received_iso','device_time_us','zone','candidate_zone','peak_adc','intensity','ambiguous','clipped','sensor_1','sensor_2','sensor_3','sensor_4']];
  for (const e of [...events].reverse()) rows.push([new Date(e.time).toISOString(), e.timeUs ?? '', e.zone, e.candidate ?? '', e.peak, e.intensity.toFixed(3), e.ambiguous, e.clipped, ...e.readings.map(v=>v ?? '')]);
  return rows.map(row=>row.map(v=>'"'+String(v).replaceAll('"','""')+'"').join(',')).join('\r\n');
}
