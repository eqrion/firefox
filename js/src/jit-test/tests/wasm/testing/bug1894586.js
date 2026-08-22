// |jit-test| skip-if: true
//
// wasmDumpIon is disabled on the itasca branch, and this module's
// struct type is post-1.0 in any case.

var it = 100;
function f() {
  if (--it < 0) {
    return;
  }
  wasmDumpIon(
    wasmTextToBinary(
      "(type $x (struct))(global $g (ref null $x) ref.null $x)(func $h)"
    )
  );
  oomTest(f);
}
f();
