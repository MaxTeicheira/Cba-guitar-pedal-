// Render a parity scenario through the JS reference core.
//   node tests/parity/render.js <scenario.txt> <out.f32>
// Output: interleaved stereo float32 little-endian.
'use strict';
const fs = require('fs');
const { BadMoodCore, PRESET_KEYS } = require('../../src/badmood-worklet.js');
const { parse, input, BLOCK } = require('./scenario.js');

const sc = parse(fs.readFileSync(process.argv[2], 'utf8'));
const core = new BadMoodCore(sc.rate);
const nBlocks = Math.ceil((sc.seconds * sc.rate) / BLOCK);
const out = new Float32Array(nBlocks * BLOCK * 2);
const inp = new Float32Array(BLOCK), inR = new Float32Array(BLOCK), L = new Float32Array(BLOCK), R = new Float32Array(BLOCK);
for (let b = 0; b < nBlocks; b++) {
  for (const ev of sc.events.get(b) || []) {
    if (ev.kind === 'param') {
      if (!PRESET_KEYS.includes(ev.name)) { console.error(`unknown param ${ev.name}`); process.exit(2); }
      core.setParam(ev.name, ev.value);
    } else if (ev.kind === 'cmd') core.command(ev.name, ev.value);
    else core.footswitch(ev.which, ev.action);
  }
  for (let i = 0; i < BLOCK; i++) { inp[i] = input(b * BLOCK + i, sc.rate); inR[i] = 0.8 * input(b * BLOCK + i + 7919, sc.rate); }
  core.process(inp, sc.stereo ? inR : null, L, R);
  for (let i = 0; i < BLOCK; i++) { out[2 * (b * BLOCK + i)] = L[i]; out[2 * (b * BLOCK + i) + 1] = R[i]; }
}
fs.writeFileSync(process.argv[3], Buffer.from(out.buffer));
