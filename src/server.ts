// SmartDesk Hub – verbindet sich mit dem ESP32, speichert Verlauf, spielt lauten Alarm am PC.
import http from 'node:http';
import { exec, type ChildProcess } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ESP = (process.env.ESP_URL ?? 'http://smartdesk.local').replace(/\/$/, '');
const PORT = Number(process.env.PORT ?? 3000);
const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const DB_FILE = join(ROOT, 'db.json');

interface Esp { s: 'idle' | 'focus' | 'break'; l: string; set: number; rem: number; len: number; src: number;
  raw: number; temp: number; rssi: number; up: number; heap: number; alarm: number; at: string; ip: string; fast: number }
interface Entry { t: number; type: 'focus' | 'abort' | 'break'; min: number }
interface DB { entries: Entry[]; outages: { from: number; to?: number }[] }
type Ring = 'break' | 'ready' | null;

const db: DB = existsSync(DB_FILE) ? JSON.parse(readFileSync(DB_FILE, 'utf8')) : { entries: [], outages: [] };
const save = () => writeFileSync(DB_FILE, JSON.stringify(db));

let last: Esp | null = null, prev: Esp | null = null;
let online = false, fails = 0, latency = 0, ringing: Ring = null;
let ringTimer: NodeJS.Timeout | undefined;
const hist = { lat: [] as number[], temp: [] as number[] };
const clients = new Set<http.ServerResponse>();

// ---------- Alarm: Systemton am PC (laut, auch wenn der Browser im Hintergrund ist) ----------
const procs = new Set<ChildProcess>();
function sound(): void {
  const cmd = process.platform === 'win32'
    ? `powershell -NoProfile -c "$p=New-Object Media.SoundPlayer 'C:\\Windows\\Media\\Alarm02.wav';$p.PlaySync()"`
    : process.platform === 'darwin'
      ? 'afplay /System/Library/Sounds/Sosumi.aiff'
      : 'paplay /usr/share/sounds/freedesktop/stereo/alarm-clock-elapsed.oga';
  const c: ChildProcess = exec(cmd, () => procs.delete(c));
  procs.add(c);
}
function ring(type: Ring): void {
  ringing = type; clearInterval(ringTimer); let n = 0; sound();
  ringTimer = setInterval(() => { if (!ringing || ++n >= 6) return clearInterval(ringTimer); if (!procs.size) sound(); }, 5000);
}
function ack(): void {
  ringing = null; clearInterval(ringTimer);
  for (const p of procs) { if (process.platform === 'win32' && p.pid) exec(`taskkill /pid ${p.pid} /T /F`, () => {}); else p.kill(); }
  procs.clear();
}

// ---------- Verlauf erkennen ----------
function track(s: Esp): void {
  if (prev) {
    if (s.alarm > prev.alarm) {   // nur echte neue Alarme (kein Fehlalarm nach ESP32-Neustart)
      if (!prev.fast) {
        if (s.at === 'break') db.entries.push({ t: Date.now(), type: 'focus', min: prev.set });
        if (s.at === 'ready') db.entries.push({ t: Date.now(), type: 'break', min: Math.round(prev.len / 60) });
        save();
      }
      ring(s.at === 'break' ? 'break' : 'ready');
    } else if (prev.s === 'focus' && s.s === 'idle' && !prev.fast) {
      const min = Math.floor((prev.len - prev.rem) / 60);
      if (min > 0) { db.entries.push({ t: Date.now(), type: 'abort', min }); save(); }
    }
  }
  prev = s;
}

function stats() {
  const day = (t: number) => new Date(t).toLocaleDateString('sv');
  const per: Record<string, number> = {};
  for (const e of db.entries) if (e.type !== 'break') per[day(e.t)] = (per[day(e.t)] ?? 0) + e.min;
  const days = Array.from({ length: 14 }, (_, i) => {
    const d = day(Date.now() - (13 - i) * 864e5);
    return { d, min: per[d] ?? 0 };
  });
  const f = db.entries.filter(e => e.type === 'focus'), a = db.entries.filter(e => e.type === 'abort');
  return {
    days, today: days[13].min, week: days.slice(7).reduce((x, d) => x + d.min, 0),
    sessions: f.length, aborted: a.length,
    avg: f.length ? Math.round(f.reduce((x, e) => x + e.min, 0) / f.length) : 0,
    longest: Math.max(0, ...f.map(e => e.min)),
    outages: db.outages.length, recent: db.entries.slice(-12).reverse(),
  };
}

function push(): void {
  const data = `data: ${JSON.stringify({ esp: last, online, latency, ringing, hist, stats: stats() })}\n\n`;
  for (const c of clients) c.write(data);
}

// ---------- ESP32 abfragen ----------
async function poll(): Promise<void> {
  const t = performance.now();
  try {
    const r = await fetch(`${ESP}/api/state`, { signal: AbortSignal.timeout(2500) });
    const s = (await r.json()) as Esp;
    latency = Math.round(performance.now() - t); fails = 0;
    if (!online) {
      online = true; const o = db.outages.at(-1);
      if (o && !o.to) { o.to = Date.now(); save(); }
      console.log(`ESP32 online (${s.ip})`);
    }
    track(s); last = s;
    hist.lat.push(latency); hist.temp.push(s.temp);
    for (const k of ['lat', 'temp'] as const) if (hist[k].length > 60) hist[k].shift();
  } catch {
    if (++fails >= 3 && online) { online = false; db.outages.push({ from: Date.now() }); save(); console.log('ESP32 offline'); }
  }
  push();
}
setInterval(poll, 1000);

// ---------- HTTP ----------
const CMDS = new Set(['start', 'stop', 'set', 'fast', 'ledtest']);
http.createServer(async (req, res) => {
  const u = new URL(req.url ?? '/', 'http://localhost');
  if (u.pathname === '/events') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store', Connection: 'keep-alive' });
    clients.add(res); req.on('close', () => clients.delete(res)); push(); return;
  }
  if (u.pathname === '/ack') { ack(); push(); res.end('ok'); return; }
  if (u.pathname === '/test-alarm') { ring('break'); push(); res.end('ok'); return; }
  if (u.pathname.startsWith('/esp/')) {
    const cmd = u.pathname.slice(5);
    if (!CMDS.has(cmd)) { res.writeHead(404).end(); return; }
    try { await fetch(`${ESP}/api/${cmd}${u.search}`, { signal: AbortSignal.timeout(2500) }); res.end('ok'); }
    catch { res.writeHead(502).end('ESP32 offline'); }
    return;
  }
  res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' }).end(readFileSync(join(ROOT, 'public', 'index.html')));
}).listen(PORT, '127.0.0.1', () => console.log(`SmartDesk Hub: http://localhost:${PORT}  ·  ESP32: ${ESP}`));
