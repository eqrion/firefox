// |jit-test| skip-if: !wasmIsSupported() || wasmCompileMode() != "baseline+ion"
//
// Tier-2 recompiles a body by driving itasca over the retained code section
// again, which needs the body's entry offset. That offset is relative to the
// section, because the section is the region tier-1 drove itasca over. A
// tier-2 path that re-bases it a second time hands itasca a position past the
// end of the section, itasca reports "unexpected end of input", and tier-2
// fails.
//
// Nothing observable breaks when it does. A failed tier-2 only warns, and
// wasmHasTier2CompilationCompleted still returns true, because the generator
// task's destructor clears the flag whether or not the task succeeded. So this
// asks the one question that tells the two apart: what tier did the function
// actually end up at.
//
// The module has a type and a function section ahead of its code section, so
// the section does not start at module offset 0 and the two frames differ.
// That is true of every real module; a test that did not check the tier would
// pass either way.

const text = `(module
  (func (export "f") (param i32) (param i32) (result i32)
    local.get 0
    local.get 1
    i32.add
    local.get 0
    i32.mul
  )
)`;

// Complete tier-2: the whole module is recompiled at once, off thread.
function completeTier2() {
  const module = new WebAssembly.Module(wasmTextToBinary(text));
  const { f } = new WebAssembly.Instance(module).exports;
  assertEq(f(3, 4), 21);

  while (!wasmHasTier2CompilationCompleted(module)) {
    sleep(0.01);
  }

  assertEq(wasmFunctionTier(f), 'optimized');
  assertEq(f(3, 4), 21);
}

// Partial tier-2: one function is recompiled when it gets hot.
function partialTier2() {
  const { f } = new WebAssembly.Instance(
      new WebAssembly.Module(wasmTextToBinary(text))).exports;

  // Bounded rather than a bare spin, so a dropped tier-up request fails the
  // assertion below instead of hanging. A dropped request is exactly what a
  // failing partial tier-2 looks like: WasmCode.cpp leaves the function in
  // TierUpState::Requested forever.
  for (let i = 0; i < 200000 && wasmFunctionTier(f) !== 'optimized'; i++) {
    assertEq(f(3, 4), 21);
  }

  assertEq(wasmFunctionTier(f), 'optimized');
  assertEq(f(3, 4), 21);
}

if (wasmLazyTieringEnabled()) {
  partialTier2();
} else {
  completeTier2();
}
