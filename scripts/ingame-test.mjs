// In-game spike: records League through game_capture as a vod only (no ring), samples the Live Client API game
// clock so the anchor can be checked against the HUD timer afterwards, and checks the protocol 2 basics.
//   node scripts/ingame-test.mjs [--seconds 120] [--mic <deviceId>] [--out <dir>] [--source game|monitor] [--eof]
// --source monitor records the screen instead (no game needed, nothing injected). --eof closes stdin instead of
// sending stop, as a crashed app would: the vod must still be finalised.
// Ctrl+C stops early. Writes <out>/ingame.mp4 and <out>/session.json. Probes the vod with obs-deps' ffprobe if built.
import { spawn, spawnSync } from 'node:child_process';
import { closeSync, existsSync, mkdirSync, openSync, readdirSync, readSync, rmSync, statSync, writeFileSync } from 'node:fs';
import https from 'node:https';
import { dirname, join, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, all) => (a.startsWith('--') ? [...acc, [a.slice(2), all[i + 1]]] : acc), []),
);
const seconds = Number(args.seconds ?? 120);
const source = args.source ?? 'game';
const eof = 'eof' in args;
const fps = 60;
const keyframeSec = 2;
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const outDir = resolve(args.out ?? join(root, 'build', 'ingame'));
mkdirSync(outDir, { recursive: true });
const vod = join(outDir, 'ingame.mp4');
rmSync(vod, { force: true });
const host = join(root, 'build', 'obs-install', 'bin', '64bit', 'recorder-host.exe');

const session = { startedAt: Date.now(), events: [], anchors: [], progress: [], clock: [] };
const proc = spawn(host, [], { stdio: ['pipe', 'pipe', 'inherit'] });
const send = (o) => proc.stdin.write(JSON.stringify(o) + '\n');
let stopping = false;

createInterface({ input: proc.stdout }).on('line', (line) => {
  let ev;
  try {
    ev = JSON.parse(line);
  } catch {
    console.log(`[test] not JSON: ${line}`);
    return;
  }
  ev.receivedAt = Date.now();
  if (ev.event === 'anchor') session.anchors.push(ev);
  else if (ev.event === 'progress') session.progress.push(ev);
  else session.events.push(ev);
  if (ev.event !== 'levels') console.log(`[host] ${line}`);
  if (ev.event === 'ready') send({ cmd: 'info', id: 1 });
  if (ev.event === 'info') {
    send({
      cmd: 'start',
      id: 2,
      output: { vod: vod.replace(/\\/g, '/') },
      video: { source, width: 1920, height: 1080, fps, keyframeSec },
      audio: [
        { id: 'game', kind: 'process', executable: 'League of Legends.exe' },
        ...(args.mic ? [{ id: 'mic', kind: 'input', deviceId: args.mic }] : []),
      ],
    });
    setTimeout(stop, seconds * 1000);
  }
  if (ev.event === 'error' && ev.cmd === 'start') send({ cmd: 'shutdown' });
  if (ev.event === 'stopped') send({ cmd: 'shutdown' });
});

function stop() {
  if (stopping) return;
  stopping = true;
  console.log(eof ? '[test] closing stdin' : '[test] stopping');
  if (eof) proc.stdin.end();
  else send({ cmd: 'stop', id: 3 });
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

// Top-level MP4 boxes. A finalised Hybrid MP4 is ftyp, mdat, moov; one that was not still has its moof fragments.
function topLevelBoxes(file) {
  const fd = openSync(file, 'r');
  const header = Buffer.alloc(16);
  const boxes = [];
  try {
    for (let pos = 0, end = statSync(file).size; pos + 8 <= end; ) {
      readSync(fd, header, 0, 16, pos);
      let size = header.readUInt32BE(0);
      if (size === 1) size = Number(header.readBigUInt64BE(8));
      else if (size === 0) size = end - pos;
      if (size < 8) break;
      boxes.push(header.toString('latin1', 4, 8));
      pos += size;
    }
  } finally {
    closeSync(fd);
  }
  return boxes;
}

function check() {
  const results = [];
  const expect = (name, ok, detail = '') => results.push({ name, ok: Boolean(ok), detail });
  const find = (event) => session.events.find((e) => e.event === event);

  const info = find('info');
  expect('info.protocol is 2', info?.protocol === 2, `protocol=${info?.protocol}`);
  const features = info?.features ?? [];
  expect('info.features has H1', ['vod-only', 'progress', 'encoder-settings'].every((f) => features.includes(f)), features.join(','));

  const starting = find('starting');
  expect('starting without a ring', starting && starting.path === '' && starting.vod !== '', `path="${starting?.path}" vod="${starting?.vod}"`);
  expect('started', find('started'));
  expect('no segment event', !session.events.some((e) => e.event === 'segment'));
  expect('no ring file', !readdirSync(outDir).some((f) => f.endsWith('.ts')));

  const first = session.anchors[0];
  // Anchors are measured on the vod: its first keyframe is the file's first frame, at file time 0.
  expect('first anchor at vod time 0', first?.kind === 'first' && Math.abs(first.ptsSec) < 1 / fps, `ptsSec=${first?.ptsSec}`);

  const ends = session.progress.map((p) => p.endWallMs);
  const gaps = ends.slice(1).map((e, i) => (e - ends[i]) / 1000);
  const sorted = [...gaps].sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)];
  expect('progress every keyframeSec', gaps.length > 0 && Math.abs(median - keyframeSec) < 0.25, `${ends.length} events, median gap ${median?.toFixed(3)} s`);
  expect('progress after the first anchor', first && ends.length > 0 && ends[0] > first.wallMs);

  const stopped = find('stopped');
  const lastProgress = ends.at(-1) ?? 0;
  if (eof) {
    // Nobody reads stdout after EOF: the run ends at its last progress, as the app's recovery assumes.
    expect('vod stopped on EOF', session.events.some((e) => e.event === 'output_stopped' && e.output === 'vod' && e.code === 0));
  } else {
    expect('stopped.endWallMs after the last progress', stopped?.endWallMs >= lastProgress, `endWallMs=${stopped?.endWallMs}`);
  }
  const endWallMs = eof ? lastProgress : stopped?.endWallMs;
  if (first && endWallMs) {
    const startWallMs = first.wallMs - first.ptsSec * 1000;
    session.runDurationSec = (endWallMs - startWallMs) / 1000;
  }
  expect('vod written', existsSync(vod) && statSync(vod).size > 0, existsSync(vod) ? `${statSync(vod).size} bytes` : 'missing');
  if (existsSync(vod)) {
    const boxes = topLevelBoxes(vod);
    expect('vod finalised', !boxes.includes('moof') && boxes.at(-1) === 'moov', boxes.join(' '));
  }

  const deps = join(root, 'obs-studio', '.deps');
  const depsDir = existsSync(deps) ? readdirSync(deps).find((d) => /^obs-deps-20.*-x64$/.test(d)) : undefined;
  const ffprobe = depsDir && join(deps, depsDir, 'bin', 'ffprobe.exe');
  if (ffprobe && existsSync(ffprobe) && existsSync(vod)) {
    const probe = spawnSync(ffprobe, [
      '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=duration,nb_frames', '-of', 'json', vod,
    ]);
    const stream = JSON.parse(probe.stdout.toString() || '{}').streams?.[0];
    const fileSec = Number(stream?.duration);
    session.vodDurationSec = fileSec;
    expect('vod readable', probe.status === 0 && fileSec > 0, `${fileSec} s, ${stream?.nb_frames} frames`);
    if (!eof && session.runDurationSec) {
      // Exact but for clock conversion noise and ffprobe rounding.
      expect('stopped.endWallMs matches the vod end', Math.abs(session.runDurationSec - fileSec) < 0.002,
        `anchors say ${session.runDurationSec.toFixed(3)} s, the file ${fileSec} s`);
    } else if (session.runDurationSec) {
      expect('last progress within the vod', session.runDurationSec <= fileSec, `progress says ${session.runDurationSec.toFixed(3)} s, the file ${fileSec} s`);
    }
  }

  for (const r of results) console.log(`[test] ${r.ok ? 'ok  ' : 'FAIL'} ${r.name}${r.detail ? `: ${r.detail}` : ''}`);
  session.checks = results;
  return results.every((r) => r.ok);
}

proc.on('exit', (code) => {
  clearInterval(poll);
  const ok = check();
  writeFileSync(join(outDir, 'session.json'), JSON.stringify(session, null, 2));
  const hooked = session.events.some((e) => e.event === 'hooked');
  console.log(
    `[test] host exit ${code}; hooked=${hooked}; anchors=${session.anchors.length}; progress=${session.progress.length}; clock samples=${session.clock.length}`,
  );
  console.log(`[test] ${outDir}`);
  process.exitCode = ok && code === 0 ? 0 : 1;
});
