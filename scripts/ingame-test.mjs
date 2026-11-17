// In-game spike: records League through game_capture as a vod only (no ring), samples the Live Client API game
// clock so the anchor can be checked against the HUD timer afterwards, and checks the protocol basics (H1), the
// Preview tiles, Sprite sheets and Thumbnails (H2), and Clips (H3): two cut from the vod while it is written (its
// start, and seconds just before the last progress), the second cut again from the finalised vod, and the errors.
//   node scripts/ingame-test.mjs [--seconds 120] [--mic <deviceId>] [--out <dir>] [--source game|monitor] [--eof]
//                                [--columns 10] [--rows 10]
// --source monitor records the screen instead (no game needed, nothing injected). --eof closes stdin instead of
// sending stop, as a crashed app would: the vod must still be finalised.
// --columns and --rows set the Sprite sheet grid (a small one checks the move to the next sheet in a short run).
// Ctrl+C stops early. Writes <out>/ingame.mp4, <out>/sheets/sheet_*.jpg, <out>/thumbnail.jpg, <out>/clips/*.mp4,
// <out>/session.json. Probes the vod, the sheets, the Thumbnail and the Clips with obs-deps' ffprobe if built.
// Needs --seconds 20 or more for the live Clips.
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
const preview = {
  prefix: 'sheet',
  tileWidth: 160,
  tileHeight: 90,
  intervalMs: 2000,
  columns: Number(args.columns ?? 10),
  rows: Number(args.rows ?? 10),
  flushTiles: 5,
};
const thumbnailSize = { width: 480, height: 270 };
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const outDir = resolve(args.out ?? join(root, 'build', 'ingame'));
mkdirSync(outDir, { recursive: true });
const vod = join(outDir, 'ingame.mp4');
rmSync(vod, { force: true });
const sheetDir = join(outDir, 'sheets');
rmSync(sheetDir, { recursive: true, force: true });
mkdirSync(sheetDir, { recursive: true });
const thumbnail = join(outDir, 'thumbnail.jpg');
rmSync(thumbnail, { force: true });
const clipDir = join(outDir, 'clips');
rmSync(clipDir, { recursive: true, force: true });
// Clip ids: 10 the vod's start and 11 just before the last progress, both live; 12 the range of 11 from the
// finalised vod; 13 an empty range; 14 a missing source.
const clipPath = (id) => join(clipDir, `clip_${id}.mp4`);
const clipsAsked = {};
const slash = (path) => path.replace(/\\/g, '/');
const host = join(root, 'build', 'obs-install', 'bin', '64bit', 'recorder-host.exe');

const session = { startedAt: Date.now(), events: [], anchors: [], progress: [], previews: [], clock: [], sheetSizes: {} };
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
  else if (ev.event === 'preview') session.previews.push(ev);
  else session.events.push(ev);
  if (ev.event !== 'levels') console.log(`[host] ${line}`);
  if (ev.event === 'ready') send({ cmd: 'info', id: 1 });
  if (ev.event === 'info') {
    send({
      cmd: 'start',
      id: 2,
      output: { vod: slash(vod) },
      video: { source, width: 1920, height: 1080, fps, keyframeSec },
      preview: { directory: slash(sheetDir), ...preview },
      thumbnail: { path: slash(thumbnail), ...thumbnailSize },
      audio: [
        { id: 'game', kind: 'process', executable: 'League of Legends.exe' },
        ...(args.mic ? [{ id: 'mic', kind: 'input', deviceId: args.mic }] : []),
      ],
    });
    setTimeout(stop, seconds * 1000);
    // A Thumbnail on command halfway, to the start.thumbnail path, timed from the command to its reply.
    setTimeout(() => {
      if (stopping) return;
      session.thumbnailSentAt = Date.now();
      send({ cmd: 'thumbnail', id: 4 });
    }, (seconds * 1000) / 2);
    // Clips of the vod being written, at two thirds of the run, from the last progress in file time.
    setTimeout(() => {
      const first = session.anchors[0];
      const last = session.progress.at(-1);
      if (stopping || !first || !last) return;
      const t = (last.endWallMs - (first.wallMs - first.ptsSec * 1000)) / 1000;
      clip(10, vod, 0.5, 3);
      clip(11, vod, Math.max(0, t - 8), t - 3);
      clip(13, vod, 5, 5);
      clip(14, join(outDir, 'missing.mp4'), 0, 5);
    }, (seconds * 1000 * 2) / 3);
  }
  if (ev.event === 'error' && ev.cmd === 'start') send({ cmd: 'shutdown' });
  // Asked once nothing records: must be refused. The finalised vod is cut once more.
  if (ev.event === 'stopped') {
    send({ cmd: 'thumbnail', id: 5 });
    if (clipsAsked[11]) clip(12, vod, clipsAsked[11].fromSec, clipsAsked[11].toSec);
  }
  if (session.events.some((e) => e.event === 'error' && e.cmd === 'thumbnail' && e.id === 5) &&
      Object.keys(clipsAsked).every((id) => replyTo(Number(id))) && !session.shutdownSentAt) {
    session.shutdownSentAt = Date.now();
    send({ cmd: 'shutdown' });
  }
});

function clip(id, source, fromSec, toSec) {
  clipsAsked[id] = { source, fromSec, toSec, sentAt: Date.now() };
  send({ cmd: 'clip', id, source: slash(source), fromSec, toSec, path: slash(clipPath(id)) });
}

function replyTo(id) {
  return session.events.find((e) => e.id === id && (e.event === 'clipped' || (e.event === 'error' && e.cmd === 'clip')));
}

function stop() {
  if (stopping) return;
  stopping = true;
  session.stopSentAt = Date.now();
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

// Sheet files while recording: each is rewritten (temp file, rename) as tiles are added, so its size grows.
const watch = setInterval(() => {
  if (stopping) return;
  for (const f of readdirSync(sheetDir).filter((name) => name.endsWith('.jpg'))) {
    const sizes = (session.sheetSizes[f] ??= []);
    const size = statSync(join(sheetDir, f), { throwIfNoEntry: false })?.size ?? 0;
    if (sizes.at(-1) !== size) sizes.push(size);
  }
}, 500);

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
  expect('info.protocol is 3', info?.protocol === 3, `protocol=${info?.protocol}`);
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

  // ---- H2 ----
  expect('info.features has H2', ['preview', 'thumbnail'].every((f) => features.includes(f)), features.join(','));
  expect('starting.preview and starting.thumbnail', starting?.preview === true && starting?.thumbnail === true,
    `preview=${starting?.preview} thumbnail=${starting?.thumbnail}`);

  const previews = session.previews;
  const firsts = new Set(previews.map((p) => p.firstWallMs));
  expect('preview events', previews.length > 0 && firsts.size === 1 && previews.every((p) => p.intervalMs === preview.intervalMs),
    `${previews.length} events, firstWallMs ${[...firsts].join(',')}`);
  // Tile 0 is the first frame after start: within a second of the vod's first frame.
  if (first && previews[0]) {
    const fromStart = previews[0].firstWallMs - (first.wallMs - first.ptsSec * 1000);
    expect('firstWallMs near the vod start', Math.abs(fromStart) < 1000, `tile 0 at ${fromStart.toFixed(1)} ms from file time 0`);
  }
  // Written every flushTiles tiles: one event per flushTiles * intervalMs while recording.
  const during = previews.filter((p) => !session.stopSentAt || p.receivedAt < session.stopSentAt);
  const eventGaps = during.slice(1).map((p, i) => (p.receivedAt - during[i].receivedAt) / 1000);
  const sortedGaps = [...eventGaps].sort((a, b) => a - b);
  const medianGap = sortedGaps[Math.floor(sortedGaps.length / 2)];
  const flushSec = (preview.flushTiles * preview.intervalMs) / 1000;
  expect('preview every flushTiles * intervalMs', eventGaps.length > 0 && Math.abs(medianGap - flushSec) < 0.5,
    `median gap ${medianGap?.toFixed(3)} s for ${flushSec} s`);
  const perSheet = preview.columns * preview.rows;
  expect('preview tiles step by flushTiles', during.every((p) => p.tiles % preview.flushTiles === 0 || p.tiles % perSheet === 0),
    during.map((p) => p.tiles).join(','));
  expect('preview.sheet and file match its tiles', previews.every((p) => p.sheet === Math.floor((p.tiles - 1) / perSheet)
    && p.file === slash(join(sheetDir, `${preview.prefix}_${String(p.sheet).padStart(6, '0')}.jpg`))));

  const lastPreview = previews.at(-1);
  if (!eof) {
    expect('stopped.tiles is the last preview tiles', stopped?.tiles === lastPreview?.tiles, `stopped.tiles=${stopped?.tiles}`);
    expect('last preview before stopped', lastPreview && stopped && lastPreview.receivedAt <= stopped.receivedAt);
    if (lastPreview && stopped?.endWallMs) {
      // Tiles stop when stop is asked, a little before the video ends: at most one tile short of the end.
      const expected = Math.floor((stopped.endWallMs - lastPreview.firstWallMs) / preview.intervalMs) + 1;
      expect('stopped.tiles matches the duration', stopped.tiles <= expected && stopped.tiles >= expected - 1,
        `${stopped.tiles} tiles, ${expected} for ${((stopped.endWallMs - lastPreview.firstWallMs) / 1000).toFixed(3)} s`);
    }
  }
  const sheets = readdirSync(sheetDir).filter((f) => f.endsWith('.jpg')).sort();
  expect('one sheet per started grid', lastPreview && sheets.length === Math.ceil(lastPreview.tiles / perSheet), sheets.join(','));
  expect('no temp file left', !readdirSync(sheetDir).some((f) => f.endsWith('.tmp')) && !existsSync(thumbnail + '.tmp'));
  const grew = Object.entries(session.sheetSizes).filter(([, sizes]) => sizes.filter((n) => n > 0).length > 1);
  expect('sheets grow while recording', grew.length > 0,
    Object.entries(session.sheetSizes).map(([f, sizes]) => `${f}: ${sizes.join(' ')}`).join('; '));

  const autoThumb = session.events.find((e) => e.event === 'thumbnail' && e.id === undefined);
  expect('automatic thumbnail', autoThumb?.path === slash(thumbnail) && first && Math.abs(autoThumb.wallMs - first.wallMs) < 1000,
    autoThumb ? `wallMs ${(autoThumb.wallMs - first?.wallMs).toFixed(1)} ms from the first anchor` : 'missing');
  const commandThumb = session.events.find((e) => e.event === 'thumbnail' && e.id === 4);
  if (session.thumbnailSentAt) {
    const latency = commandThumb ? commandThumb.receivedAt - session.thumbnailSentAt : NaN;
    session.thumbnailCommandMs = latency;
    expect('thumbnail command', commandThumb?.path === slash(thumbnail) && latency < 50, `${latency} ms command to reply (budget 50)`);
  }
  // After EOF nothing can be asked any more.
  if (!eof) expect('thumbnail refused when not recording',
    session.events.some((e) => e.event === 'error' && e.cmd === 'thumbnail' && e.id === 5 && e.message === 'not recording'));
  expect('thumbnail file', existsSync(thumbnail) && statSync(thumbnail).size > 0);

  if (ffprobe && existsSync(ffprobe)) {
    const probeImage = (file) => {
      const probe = spawnSync(ffprobe, ['-v', 'error', '-show_entries', 'stream=codec_name,width,height', '-of', 'json', file]);
      return JSON.parse(probe.stdout.toString() || '{}').streams?.[0];
    };
    const sheetW = preview.columns * preview.tileWidth;
    const sheetH = preview.rows * preview.tileHeight;
    const probed = sheets.map((f) => ({ f, s: probeImage(join(sheetDir, f)) }));
    expect('sheets are JPEGs of the grid size',
      probed.length > 0 && probed.every(({ s }) => s?.codec_name === 'mjpeg' && s.width === sheetW && s.height === sheetH),
      probed.map(({ f, s }) => `${f} ${s?.codec_name} ${s?.width}x${s?.height} ${statSync(join(sheetDir, f)).size} B`).join('; '));
    const t = existsSync(thumbnail) ? probeImage(thumbnail) : undefined;
    expect('thumbnail is a JPEG of its size',
      t?.codec_name === 'mjpeg' && t.width === thumbnailSize.width && t.height === thumbnailSize.height,
      `${t?.codec_name} ${t?.width}x${t?.height} ${existsSync(thumbnail) ? statSync(thumbnail).size : 0} B`);
  }

  // ---- H3 ----
  expect('info.features has clip', features.includes('clip'), features.join(','));
  const probeJson = (file, entries, extra = []) => {
    const probe = spawnSync(ffprobe, ['-v', 'error', ...extra, '-show_entries', entries, '-of', 'json', file]);
    return probe.status === 0 ? JSON.parse(probe.stdout.toString() || '{}') : undefined;
  };
  const vodStreams = ffprobe && existsSync(ffprobe) && existsSync(vod)
    ? (probeJson(vod, 'stream=codec_type')?.streams ?? []).map((s) => s.codec_type).join(',')
    : undefined;
  const checkClip = (id, label) => {
    const asked = clipsAsked[id];
    if (!asked) {
      expect(`clip ${label}`, false, 'not asked (run too short or stopped early)');
      return undefined;
    }
    const reply = replyTo(id);
    const ok = reply?.event === 'clipped';
    const latency = reply ? reply.receivedAt - asked.sentAt : NaN;
    expect(`clip ${label}`, ok && reply.path === slash(clipPath(id)) && existsSync(clipPath(id)),
      ok ? `${latency} ms, fromSec ${reply.fromSec.toFixed(3)} for ${asked.fromSec.toFixed(3)}, ${reply.durationMs} ms`
        : JSON.stringify(reply ?? 'no reply'));
    if (!ok) return undefined;
    // The keyframe at or before fromSec: never after it, never a GOP or more before it.
    expect(`clip ${label} starts on the keyframe before fromSec`,
      reply.fromSec <= asked.fromSec + 1e-6 && reply.fromSec > asked.fromSec - keyframeSec - 0.1);
    const wantMs = (asked.toSec - reply.fromSec) * 1000;
    expect(`clip ${label} durationMs covers the range`, Math.abs(reply.durationMs - wantMs) <= 100,
      `${reply.durationMs} ms for ${wantMs.toFixed(0)} ms`);
    expect(`clip ${label} leaves no .part`, !existsSync(clipPath(id) + '.part'));
    if (vodStreams !== undefined) {
      const streams = (probeJson(clipPath(id), 'stream=codec_type')?.streams ?? []).map((s) => s.codec_type).join(',');
      expect(`clip ${label} has every stream of the vod`, streams === vodStreams, `${streams} for ${vodStreams}`);
      const frame = probeJson(clipPath(id), 'frame=key_frame,pts_time', ['-select_streams', 'v:0', '-show_frames',
        '-read_intervals', '%+#1'])?.frames?.[0];
      expect(`clip ${label} decodes from its first frame`, frame?.key_frame === 1, JSON.stringify(frame));
      const duration = Number(probeJson(clipPath(id), 'format=duration')?.format?.duration);
      expect(`clip ${label} file duration`, Math.abs(duration * 1000 - reply.durationMs) <= 100,
        `${duration} s for durationMs ${reply.durationMs}`);
    }
    return reply;
  };
  checkClip(10, 'of the live vod start');
  const live = checkClip(11, 'of the live vod before the last progress');
  if (!eof) {
    const finalised = checkClip(12, 'of the finalised vod');
    if (live && finalised) {
      expect('live and finalised clips start on the same keyframe', Math.abs(live.fromSec - finalised.fromSec) < 0.002,
        `${live.fromSec} and ${finalised.fromSec}`);
    }
  }
  const errorOf = (id) => session.events.find((e) => e.event === 'error' && e.cmd === 'clip' && e.id === id);
  if (clipsAsked[13]) expect('clip empty range refused', errorOf(13)?.message === 'empty range', errorOf(13)?.message);
  if (clipsAsked[14]) expect('clip missing source refused', errorOf(14)?.message.startsWith('source unreadable'), errorOf(14)?.message);
  // Cutting must not disturb the vod: no progress gap around the live Clips beyond one keyframe interval.
  if (clipsAsked[11]) {
    const around = session.progress.filter((p) => Math.abs(p.receivedAt - clipsAsked[11].sentAt) < 10000);
    const worst = Math.max(0, ...around.slice(1).map((p, i) => (p.endWallMs - around[i].endWallMs) / 1000));
    expect('progress steady while cutting', worst < keyframeSec + 0.25, `longest gap ${worst.toFixed(3)} s`);
  }

  for (const r of results) console.log(`[test] ${r.ok ? 'ok  ' : 'FAIL'} ${r.name}${r.detail ? `: ${r.detail}` : ''}`);
  session.checks = results;
  return results.every((r) => r.ok);
}

proc.on('exit', (code) => {
  clearInterval(poll);
  clearInterval(watch);
  const ok = check();
  writeFileSync(join(outDir, 'session.json'), JSON.stringify(session, null, 2));
  const hooked = session.events.some((e) => e.event === 'hooked');
  console.log(
    `[test] host exit ${code}; hooked=${hooked}; anchors=${session.anchors.length}; progress=${session.progress.length}; clock samples=${session.clock.length}`,
  );
  console.log(`[test] ${outDir}`);
  process.exitCode = ok && code === 0 ? 0 : 1;
});
