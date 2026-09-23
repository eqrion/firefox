// Invalid compile options reject the promise instead of throwing.
const bytes = wasmTextToBinary(`(module)`);
for (const options of [1, { builtins: 1 }]) {
  for (const promise of [
    WebAssembly.compile(bytes, options),
    WebAssembly.instantiate(bytes, undefined, options),
  ]) {
    assertEq(promise instanceof Promise, true);
    assertThrowsInstanceOfAsync(() => promise, TypeError);
  }
}
