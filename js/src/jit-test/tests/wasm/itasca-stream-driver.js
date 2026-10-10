// |jit-test| skip-if: !wasmStreamingEnabled()

if (typeof setBufferStreamParams === "function") {
    setBufferStreamParams(0, 1);
}

function compileStream(bytes, valid) {
    let result, error;
    WebAssembly.compileStreaming(bytes).then(m => result = m, e => error = e);
    drainJobQueue();
    if (valid) {
        assertEq(error, undefined);
        assertEq(result instanceof WebAssembly.Module, true);
        return result;
    }
    assertEq(result, undefined);
    assertEq(error instanceof WebAssembly.CompileError, true);
}

let small = wasmTextToBinary('(module (func (export "run") (result i32) i32.const 42))');
assertEq(new WebAssembly.Instance(compileStream(small, true)).exports.run(), 42);
for (let end = 0; end < small.length; end++) {
    let prefix = small.slice(0, end);
    compileStream(prefix, WebAssembly.validate(prefix));
}

for (let text of ['(module)', '(module (memory (export "mem") 1) (data (i32.const 0) "x"))']) {
    let instance = new WebAssembly.Instance(compileStream(wasmTextToBinary(text), true));
    if (instance.exports.mem) {
        assertEq(new Uint8Array(instance.exports.mem.buffer)[0], 120);
    }
}

for (let bytes of [small, wasmTextToBinary('(module (memory 1) (data (i32.const 0) "x"))')]) {
    let custom = new Uint8Array(bytes.length + 6);
    custom.set(bytes);
    custom.set([0, 4, 1, 120, 121, 122], bytes.length);
    for (let module of [new WebAssembly.Module(custom), compileStream(custom, true)]) {
        let sections = WebAssembly.Module.customSections(module, 'x');
        assertEq(sections.length, 1);
        assertEq(new Uint8Array(sections[0]).join(','), '121,122');
    }
}

let text = '(module (memory (export "mem") 1)';
for (let i = 0; i < 2200; i++) {
    text += `(func ${i === 2199 ? '(export "run")' : ''} (result i32) ${'nop '.repeat(64)} i32.const ${i})`;
}
text += '(data (i32.const 0) "ok"))';
let large = wasmTextToBinary(text);
if (typeof setBufferStreamParams === "function") {
    setBufferStreamParams(0, 257);
}
let instance = new WebAssembly.Instance(compileStream(large, true));
assertEq(instance.exports.run(), 2199);
assertEq(new Uint8Array(instance.exports.mem.buffer)[0], 111);

// An invalid tail can arrive with compiler tasks still active.
let badTail = new Uint8Array(large.length + 2);
badTail.set(large);
badTail.set([13, 0], large.length);
assertEq(WebAssembly.validate(badTail), false);
compileStream(badTail, false);

// Replace a late body's opcode without changing its framing.
let badBody = large.slice();
let nop = 0;
for (let i = 0; i < badBody.length; i++) {
    if (badBody[i] === 1 && badBody[i + 1] === 1 && badBody[i + 2] === 1) { nop = i; }
}
badBody[nop] = 255;
assertEq(WebAssembly.validate(badBody), false);
compileStream(badBody, false);

for (let missing of [1, 10, 100, 1000]) {
    let prefix = large.slice(0, large.length - missing);
    compileStream(prefix, WebAssembly.validate(prefix));
}
