// Assembles what DPM ships from build/obs-install: the host, libobs, the plugins it allows, their
// data, and the obs-deps DLLs those binaries import (walked with dumpbin, never listed by hand).
//   node scripts/dist.mjs <obs-install> <obs-deps/bin> <dumpbin.exe> <out-dir>
import { execFileSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, readdirSync, rmSync, statSync } from 'node:fs';
import { basename, dirname, join } from 'node:path';

const [installDir, depsBinDir, dumpbin, outDir] = process.argv.slice(2);
if (!installDir || !depsBinDir || !dumpbin || !outDir) {
  console.error('usage: dist.mjs <obs-install> <obs-deps/bin> <dumpbin.exe> <out-dir>');
  process.exit(1);
}

// Keep in step with the allowlist in src/recorder.cpp.
const PLUGINS = ['win-capture', 'win-wasapi', 'obs-ffmpeg', 'obs-outputs', 'obs-nvenc', 'obs-qsv11', 'obs-x264'];
// Spawned by libobs or the plugins, so no import points at them.
const HELPERS = ['recorder-host.exe', 'obs-ffmpeg-mux.exe', 'obs-nvenc-test.exe', 'obs-amf-test.exe', 'obs-qsv-test.exe'];
// Loaded by name at runtime (graphics module, WGC fallback in win-capture).
const RUNTIME_DLLS = ['libobs-d3d11.dll', 'libobs-winrt.dll'];

rmSync(outDir, { recursive: true, force: true });
const bin = join(outDir, 'bin', '64bit');
const plugins = join(outDir, 'obs-plugins', '64bit');
mkdirSync(bin, { recursive: true });
mkdirSync(plugins, { recursive: true });

const copy = (from, to) => {
  mkdirSync(dirname(to), { recursive: true });
  cpSync(from, to, { recursive: true, filter: (src) => !src.endsWith('.pdb') });
};

const installBin = join(installDir, 'bin', '64bit');
const roots = [];
for (const file of [...HELPERS, ...RUNTIME_DLLS]) {
  const source = join(installBin, file);
  if (!existsSync(source)) throw new Error(`missing ${source}`);
  copy(source, join(bin, file));
  roots.push(join(bin, file));
}
for (const plugin of PLUGINS) {
  const source = join(installDir, 'obs-plugins', '64bit', `${plugin}.dll`);
  copy(source, join(plugins, `${plugin}.dll`));
  roots.push(join(plugins, `${plugin}.dll`));
  const data = join(installDir, 'data', 'obs-plugins', plugin);
  if (existsSync(data)) copy(data, join(outDir, 'data', 'obs-plugins', plugin));
}
copy(join(installDir, 'data', 'libobs'), join(outDir, 'data', 'libobs'));

// Walk imports: libobs and obs-deps DLLs are resolved from the install, then from obs-deps.
const candidates = new Map();
for (const dir of [installBin, depsBinDir]) {
  for (const file of readdirSync(dir)) {
    if (/\.dll$/i.test(file) && !candidates.has(file.toLowerCase())) {
      candidates.set(file.toLowerCase(), join(dir, file));
    }
  }
}
const imports = (file) =>
  [...execFileSync(dumpbin, ['/nologo', '/dependents', file], { encoding: 'utf8' }).matchAll(/^\s+(\S+\.dll)\s*$/gim)].map(
    (match) => match[1].toLowerCase(),
  );
const seen = new Set(roots.map((root) => basename(root).toLowerCase()));
const queue = [...roots];
while (queue.length) {
  for (const dll of imports(queue.pop())) {
    if (seen.has(dll) || !candidates.has(dll)) continue;
    seen.add(dll);
    const target = join(bin, basename(candidates.get(dll)));
    copy(candidates.get(dll), target);
    queue.push(target);
  }
}

const size = (dir) =>
  readdirSync(dir, { withFileTypes: true }).reduce(
    (sum, entry) => sum + (entry.isDirectory() ? size(join(dir, entry.name)) : statSync(join(dir, entry.name)).size),
    0,
  );
console.log(`dist: ${outDir} (${(size(outDir) / 1024 / 1024).toFixed(1)} MB)`);
