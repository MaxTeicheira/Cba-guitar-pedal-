// Render every scenario with the JS reference and the C++ port and compare.
//   node tests/parity/compare.js <path to mood_render executable>
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const exe = process.argv[2];
const dir = path.join(__dirname, 'scenarios');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mood-parity-'));
let failed = 0;
for (const f of fs.readdirSync(dir).filter((x) => x.endsWith('.txt')).sort()) {
  const sc = path.join(dir, f);
  const a = path.join(tmp, f + '.js.f32'), b = path.join(tmp, f + '.cpp.f32');
  execFileSync(process.execPath, [path.join(__dirname, 'render.js'), sc, a]);
  execFileSync(exe, [sc, b]);
  const A = new Float32Array(fs.readFileSync(a).buffer.slice(0));
  const B = new Float32Array(fs.readFileSync(b).buffer.slice(0));
  if (A.length !== B.length) { console.log(`FAIL ${f}: length ${A.length} vs ${B.length}`); failed++; continue; }
  let maxDiff = 0, at = 0, sig = 0, err = 0;
  for (let i = 0; i < A.length; i++) {
    const d = Math.abs(A[i] - B[i]);
    if (d > maxDiff) { maxDiff = d; at = i; }
    sig += A[i] * A[i]; err += d * d;
  }
  const snr = 10 * Math.log10(sig / Math.max(err, 1e-30));
  // float32 buffers vs double scalars leave tiny rounding differences; anything
  // structural (a missed event, a wrong mapping) shows up as a gross error
  const ok = maxDiff < 1e-3 && snr > 80;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${f}: max |diff| ${maxDiff.toExponential(2)} at frame ${at >> 1}, SNR ${snr.toFixed(1)} dB`);
  if (!ok) failed++;
}
process.exit(failed ? 1 : 0);
