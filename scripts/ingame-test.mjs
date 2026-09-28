// In-game spike: records League through game_capture and samples the Live Client API game clock,
// so the anchor can be checked against the HUD timer afterwards.
//   node scripts/ingame-test.mjs [--seconds 120] [--mic <deviceId>] [--out <dir>]
// Ctrl+C stops early. Writes <out>/ingame.mkv and <out>/session.json.
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import https from 'node:https';
import { dirname, join, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, all) => (a.startsWith('--') ? [...acc, [a.slice(2), all[i + 1]]] : acc), []),
);
const seconds = Number(args.seconds ?? 120);
const outDir = resolve(args.out ?? join(dirname(fileURLToPath(import.meta.url)), '..', 'build', 'ingame'));
mkdirSync(outDir, { recursive: true });
const host = resolve(dirname(fileURLToPath(import.meta.url)), '..', 'build', 'obs-install', 'bin', '64bit', 'recorder-host.exe');

const session = { startedAt: Date.now(), events: [], anchors: [], clock: [] };
const proc = spawn(host, [], { stdio: ['pipe', 'pipe', 'inherit'] });
const send = (o) => proc.stdin.write(JSON.stringify(o) + '\n');
let stopping = false;

createInterface({ input: proc.stdout }).on('line', (line) => {
  const ev = JSON.parse(line);
  ev.receivedAt = Date.now();
  if (ev.event === 'anchor') session.anchors.push(ev);
  else session.events.push(ev);
  console.log(`[host] ${line}`);
  if (ev.event === 'ready') {
    send({
      cmd: 'start',
      id: 1,
      path: join(outDir, 'ingame.mkv').replace(/\\/g, '/'),
      video: { source: 'game', width: 1920, height: 1080, fps: 60 },
      audio: { game: {}, mic: { deviceId: args.mic ?? 'default' } },
    });
    setTimeout(stop, seconds * 1000);
  }
  if (ev.event === 'stopped') send({ cmd: 'shutdown' });
});

function stop() {
  if (stopping) return;
  stopping = true;
  console.log('[test] stopping');
  send({ cmd: 'stop', id: 2 });
}
process.on('SIGINT', stop);

// Game clock samples: gameTime seen between two wall readings; the tightest round trips give the best anchor.
const agent = new https.Agent({ rejectUnauthorized: false });
const poll = setInterval(() => {
  const before = Date.now();
  https
    .get('https://127.0.0.1:2999/liveclientdata/gamestats', { agent, timeout: 400 }, (res) => {
      let body = '';
      res.on('data', (c) => (body += c));
      res.on('end', () => {
        const after = Date.now();
        try {
          session.clock.push({ before, after, gameTime: JSON.parse(body).gameTime });
        } catch {}
      });
    })
    .on('error', () => {});
}, 250);

proc.on('exit', (code) => {
  clearInterval(poll);
  writeFileSync(join(outDir, 'session.json'), JSON.stringify(session, null, 2));
  const hooked = session.events.some((e) => e.event === 'hooked');
  console.log(`[test] host exit ${code}; hooked=${hooked}; anchors=${session.anchors.length}; clock samples=${session.clock.length}`);
  console.log(`[test] ${outDir}`);
});
