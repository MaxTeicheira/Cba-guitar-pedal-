// Scenario format shared by the JS and C++ renderers (see native/tests/render.cpp).
//   rate <Hz> | seconds <s> | stereo <0|1>
//   <block> param <name> <value> | <block> fs <wet|loop> <down|up|shift> | <block> cmd <name> <0|1>
'use strict';
const BLOCK = 128;

function parse(text) {
  const sc = { rate: 48000, seconds: 4, stereo: false, events: new Map() };
  for (const raw of text.split('\n')) {
    const line = raw.replace(/#.*/, '').trim();
    if (!line) continue;
    const t = line.split(/\s+/);
    if (t[0] === 'rate') sc.rate = Number(t[1]);
    else if (t[0] === 'seconds') sc.seconds = Number(t[1]);
    else if (t[0] === 'stereo') sc.stereo = Number(t[1]) !== 0;
    else {
      const b = Number(t[0]);
      let ev;
      if (t[1] === 'param') ev = { kind: 'param', name: t[2], value: Number(t[3]) };
      else if (t[1] === 'cmd') ev = { kind: 'cmd', name: t[2], value: Number(t[3]) !== 0 };
      else ev = { kind: 'fs', which: t[2], action: t[3] };
      if (!sc.events.has(b)) sc.events.set(b, []);
      sc.events.get(b).push(ev);
    }
  }
  return sc;
}

// Deterministic plucked test signal: decaying notes every 0.75 s.
function input(n, rate) {
  const t = n / rate;
  const k = Math.floor(t / 0.75);
  const f = [196, 247, 294, 330][k % 4];
  const tt = t - k * 0.75;
  return 0.35 * Math.sin(2 * Math.PI * f * t) * Math.exp(-4 * tt);
}

module.exports = { parse, input, BLOCK };
