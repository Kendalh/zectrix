// Checks the web UI's button logic (click / long press / UP+DOWN chord) with stubbed DOM and timers.
//   node sim/tests/test_buttons.js
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const html = fs.readFileSync(path.join(__dirname, '..', 'web', 'index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*)<\/script>/)[1];

let now = 0, timers = [], nextId = 1;
const fetched = [];
const handlers = {};
const els = {};
function el(id) {
  return els[id] || (els[id] = {
    id, value: '', textContent: '', scrollTop: 0, scrollHeight: 0, href: '',
    classList: { add() {}, remove() {} }, setPointerCapture() {}, blur() {},
    add() {}, addEventListener(type, fn) { (handlers[id] = handlers[id] || {})[type] = fn; },
  });
}
global.document = { getElementById: el };
global.addEventListener = (type, fn) => { (handlers.window = handlers.window || {})[type] = fn; };
global.setTimeout = (fn, ms) => { const id = nextId++; timers.push({ id, at: now + ms, fn }); return id; };
global.clearTimeout = id => { timers = timers.filter(t => t.id !== id); };
Date.now = () => now;
global.URL = { createObjectURL: () => 'blob:x' };
global.Option = function (a, b) { return { a, b }; };
global.fetch = async path => {
  fetched.push(path.replace(/[?&]t=\d+/, ''));
  return { ok: true, blob: async () => ({}), text: async () => '',
           headers: { get: h => (h === 'X-Page' ? 'home' : '1') },
           json: async () => ({ device_port: 8080, pages: [] }) };
};
new Function(script)();

function advance(ms) {
  now += ms;
  for (const t of timers.filter(t => t.at <= now).sort((a, b) => a.at - b.at)) {
    timers = timers.filter(x => x.id !== t.id);
    t.fn();
  }
}
const down = id => handlers[id].pointerdown({ pointerId: 1 });
const up = id => handlers[id].pointerup({});
async function settle() { await new Promise(r => setImmediate(r)); }
async function keysSent(action) {
  fetched.length = 0;
  action();
  await settle();
  return fetched.filter(p => p.startsWith('/key')).map(p => p.split('name=')[1]);
}

(async () => {
  await settle();  // initial /config + /reset

  assert.deepStrictEqual(await keysSent(() => { down('b-down'); advance(100); up('b-down'); }), ['down']);
  assert.deepStrictEqual(await keysSent(() => { down('b-up'); advance(100); up('b-up'); }), ['up']);
  assert.deepStrictEqual(await keysSent(() => { down('b-boot'); advance(100); up('b-boot'); }), ['boot']);

  // long press fires while held, and the release afterwards is not also a click
  assert.deepStrictEqual(await keysSent(() => { down('b-boot'); advance(1000); up('b-boot'); }), ['bootlong']);
  assert.deepStrictEqual(await keysSent(() => { down('b-down'); advance(1000); up('b-down'); }), ['downlong']);
  assert.deepStrictEqual(await keysSent(() => { down('b-up'); advance(999); up('b-up'); }), ['up']);

  // UP+DOWN held together: one chord event, no individual long presses or clicks
  assert.deepStrictEqual(await keysSent(() => {
    down('b-up'); advance(50); down('b-down'); advance(1000); up('b-up'); up('b-down');
  }), ['updownlong']);

  // after a chord, a normal click works again
  assert.deepStrictEqual(await keysSent(() => { down('b-down'); advance(100); up('b-down'); }), ['down']);

  // no double-click synthesis: two quick clicks are two clicks
  assert.deepStrictEqual(await keysSent(() => {
    down('b-boot'); advance(50); up('b-boot'); down('b-boot'); advance(50); up('b-boot');
  }), ['boot', 'boot']);

  console.log('button logic OK');
})().catch(e => { console.error(e); process.exit(1); });
