// |jit-test| skip-if: true
//
// wasmDumpIon is disabled on the itasca branch, and this module's
// struct type is post-1.0 in any case.

a = function() {
    function b(c) {
      try {
        a();
      } catch {
      }
    }
    return b;
}()

d = wasmTextToBinary("(type $x (struct))(func $h)");
a();
wasmDumpIon(d, d);
