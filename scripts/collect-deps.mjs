// Copies the obs-deps DLLs that the host, libobs and the plugins actually import into bin/64bit.
// Walks imports with dumpbin instead of a hand-written list: libcurl.dll, for one, is a hard
// import of win-capture.dll that copied lists keep missing.
import { execFileSync } from 'node:child_process';
import { copyFileSync, existsSync, readdirSync } from 'node:fs';
import { basename, join } from 'node:path';

const [installDir, depsBinDir, dumpbin] = process.argv.slice(2);
if (!installDir || !depsBinDir || !dumpbin) {
  console.error('usage: collect-deps.mjs <obs-install> <obs-deps/bin> <dumpbin.exe>');
  process.exit(1);
}

const binDir = join(installDir, 'bin', '64bit');
const pluginDir = join(installDir, 'obs-plugins', '64bit');
const available = new Map(readdirSync(depsBinDir).map((f) => [f.toLowerCase(), join(depsBinDir, f)]));

const roots = [
  ...readdirSync(binDir).filter((f) => /\.(exe|dll)$/i.test(f)).map((f) => join(binDir, f)),
  ...readdirSync(pluginDir).filter((f) => /\.dll$/i.test(f)).map((f) => join(pluginDir, f)),
];

function imports(file) {
  const out = execFileSync(dumpbin, ['/nologo', '/dependents', file], { encoding: 'utf8' });
  return [...out.matchAll(/^\s+(\S+\.dll)\s*$/gim)].map((m) => m[1].toLowerCase());
}

const seen = new Set();
const queue = [...roots];
const copied = [];
while (queue.length) {
  const file = queue.pop();
  for (const dll of imports(file)) {
    if (seen.has(dll) || !available.has(dll)) continue;
    seen.add(dll);
    const target = join(binDir, basename(available.get(dll)));
    if (!existsSync(target)) {
      copyFileSync(available.get(dll), target);
      copied.push(dll);
    }
    queue.push(target);
  }
}

console.log(`deps: ${seen.size} needed, ${copied.length} copied${copied.length ? ` (${copied.join(', ')})` : ''}`);
