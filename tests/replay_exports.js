'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const directory = process.argv[2];
const expected = fs.readFileSync(path.join(directory, 'expected.bin'));
const api = require(path.join(directory, 'replay.js'));

async function main() {
  assert.equal(api.events.length, 9);
  assert.equal(api.events[0].t, 18446744073700000000n);
  assert.deepEqual(api.events.map(e => e.type), [1, 2, 3, 2, 2, 2, 4, 5, 2]);
  let clock = 0;
  const outputs = [];
  await api.replay({now: () => clock, sleep: async ms => { clock += ms; },
    onEvent: (e, data) => {
      if (e.type === 2) outputs.push(Buffer.from(data));
      if (outputs.length === 1 && e.type === 2) clock += 2; // Slow consumer: catch up.
    }});
  assert.deepEqual(Buffer.concat(outputs), expected);
  assert.equal(clock, 4);
  await assert.rejects(api.replay({speed: 0}), /speed/);
  await assert.rejects(api.replay({onEvent: () => { throw Error('consumer'); }}), /consumer/);
  const empty = require(path.join(directory, 'empty-replay.js'));
  await empty.replay({onEvent: () => assert.fail('empty callback')});

  for (const prefix of ['', 'empty-']) {
    let instance;
    const events = [];
    const bytes = fs.readFileSync(path.join(directory, prefix + 'replay.wasm'));
    const result = await WebAssembly.instantiate(bytes, {termlib: {
      event: (type, timestamp, offset, size, rows, columns) => events.push({
        type, timestamp: BigInt.asUintN(64, timestamp), rows, columns,
        data: Buffer.from(new Uint8Array(instance.exports.memory.buffer, offset, size))})
    }});
    instance = result.instance;
    instance.exports.replay();
    assert.equal(events.length, prefix ? 0 : 9);
    if (!prefix) {
      assert(instance.exports.memory.buffer.byteLength >= 131072);
      assert.deepEqual(Buffer.concat(events.filter(e => e.type === 2).map(e => e.data)), expected);
      assert.equal(events[0].timestamp, api.events[0].t);
      assert.equal(events[2].rows, 24);
      assert.equal(events[2].columns, 80);
      instance.exports.replay();
      assert.equal(events.length, 18);
    }
  }

  globalThis.GPUBufferUsage = {COPY_DST: 8, STORAGE: 128};
  const gpu = require(path.join(directory, 'gpu.js'));
  function device(failWrite = false) {
    const buffers = [];
    let completed = 0;
    return {
      buffers, get completed() { return completed; },
      limits: {maxBufferSize: 1 << 20, maxStorageBufferBindingSize: 1 << 20},
      createBuffer(options) {
        assert.equal(options.size % 4, 0);
        assert.equal(options.usage, 136);
        const buffer = {data: new Uint8Array(options.size), destroyed: false,
          destroy() { assert(!this.destroyed); this.destroyed = true; }};
        buffers.push(buffer);
        return buffer;
      },
      queue: {
        writeBuffer(buffer, offset, bytes) {
          if (failWrite) throw Error('write failure');
          buffer.data.set(bytes, offset);
        },
        async onSubmittedWorkDone() { completed++; }
      }
    };
  }
  const target = device(), uploaded = [];
  clock = 0;
  await gpu.replay(target, (buffer, event, size) => {
    assert.equal(event.type, 2);
    assert(!buffer.destroyed);
    uploaded.push(Buffer.from(buffer.data.slice(0, size)));
    assert(buffer.data.slice(size).every(byte => byte === 0));
  }, {now: () => clock, sleep: async ms => { clock += ms; }});
  assert.deepEqual(Buffer.concat(uploaded), expected);
  assert.equal(target.buffers.length, 5);
  assert.equal(target.completed, 5);
  assert(target.buffers.every(b => b.destroyed));
  const rejected = device();
  await assert.rejects(gpu.replay(rejected, () => { throw Error('consumer failure'); }), /consumer failure/);
  assert(rejected.buffers.every(b => b.destroyed));
  const broken = device(true);
  await assert.rejects(gpu.replay(broken, () => {}), /write failure/);
  assert(broken.buffers.every(b => b.destroyed));
  const limited = device();
  limited.limits.maxBufferSize = 4;
  await assert.rejects(gpu.replay(limited, () => {}), /limit/);
  assert.equal(limited.buffers.length, 0);
  await assert.rejects(gpu.replay(target), /consumer/);
  await require(path.join(directory, 'empty-gpu.js')).replay(target, () => assert.fail('empty upload'));

  for (const prefix of ['', 'empty-']) {
    let shown = '', stopped = false;
    clock = 0;
    const context = {
      TextDecoder, Uint8Array, width: 640, height: 480,
      createCanvas(w, h) { context.width = w; context.height = h; },
      resizeCanvas(w, h) { context.width = w; context.height = h; },
      textFont() {}, textSize() {}, millis: () => clock,
      background() {}, fill() {}, text: text => { shown = text; },
      noLoop: () => { stopped = true; }
    };
    vm.createContext(context);
    vm.runInContext(fs.readFileSync(path.join(directory, prefix + 'replay.p5.js'), 'utf8'), context);
    vm.runInContext('setup()', context);
    for (clock = 0; clock <= 4 && !stopped; clock++) vm.runInContext('draw()', context);
    assert(stopped);
    if (!prefix) {
      const visible = new TextDecoder().decode(expected).slice(-65536)
        .replace(/[\x00-\x08\x0b-\x1f\x7f]/g, '\ufffd');
      assert.equal(shown, visible);
      assert.equal(context.width, 656);
      assert.equal(context.height, 400);
    } else assert.equal(shown, '');
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
