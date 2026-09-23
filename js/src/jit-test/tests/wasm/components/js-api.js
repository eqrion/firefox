// |jit-test| skip-if: !wasmComponentsEnabled()

// TODO(wasm-cm): Everything in here is nonstandard and should be bikeshedded
// internally and then eventually tested properly after discussion with the CG.
//
// Ideas for what to test later: calling without `new`, prototype chain,
// toString tag, instanceof, typeof, etc.

assertErrorMessage(() => new WebAssembly.Component(), TypeError, /1 argument required/);
assertErrorMessage(() => new WebAssembly.Component(42), TypeError, /first argument must be an ArrayBuffer/);

const bytes = wasmTextToBinary(`(component)`);
const c = new WebAssembly.Component(bytes);

let result;
WebAssembly.instantiate(c).then(r => result = r);
drainJobQueue();
assertEq(result instanceof WebAssembly.ComponentInstance, true);

WebAssembly.instantiate(bytes).then(r => result = r);
drainJobQueue();
assertEq(result.component instanceof WebAssembly.Component, true);
assertEq(result.instance instanceof WebAssembly.ComponentInstance, true);

WebAssembly.compile(bytes).then(r => result = r);
drainJobQueue();
assertEq(result instanceof WebAssembly.Component, true);
