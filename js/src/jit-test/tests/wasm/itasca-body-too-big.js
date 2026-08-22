// |jit-test| slow;
//
// wasm::MaxFunctionBytes caps a code-section entry's contents (its locals
// declarations plus its code), which is a stricter cap than itasca's own
// MAX_FUNCTION_BYTES: that one is on the code alone, after the declarations.
// WebAssembly.validate and compilation now drive the same on_code_entry hooks,
// and both apply wasm::MaxFunctionBytes to the whole entry, so a body that
// exceeds it must be rejected by both, not just by compilation.

const MaxFunctionBytes = 7654321;

function uleb(n) {
  const bytes = [];
  do {
    let byte = n % 128;
    n = Math.floor(n / 128);
    if (n !== 0) {
      byte |= 0x80;
    }
    bytes.push(byte);
  } while (n !== 0);
  return bytes;
}

function lengthPrefixed(body) {
  const lenBytes = Uint8Array.from(uleb(body.length));
  const out = new Uint8Array(lenBytes.length + body.length);
  out.set(lenBytes, 0);
  out.set(body, lenBytes.length);
  return out;
}

function section(id, body) {
  const framed = lengthPrefixed(body);
  const out = new Uint8Array(1 + framed.length);
  out[0] = id;
  out.set(framed, 1);
  return out;
}

function concat(parts) {
  const len = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(len);
  let offset = 0;
  for (const p of parts) {
    out.set(p, offset);
    offset += p.length;
  }
  return out;
}

// A few declared locals, and exactly as many `nop`s as fit under itasca's
// own cap on the code alone. The declarations push the whole entry a few
// bytes past MaxFunctionBytes while the code half stays at itasca's cap
// exactly, which is not over it: this isolates the check that has to be on
// this side, rather than also tripping itasca's.
const locals = Uint8Array.from([1, 4, /* i32 */ 0x7f]);
const nopCount = MaxFunctionBytes - 1;
const code = new Uint8Array(nopCount + 1);
code.fill(0x01, 0, nopCount);  // nop
code[code.length - 1] = 0x0b;  // end
const contents = concat([locals, code]);

const entry = lengthPrefixed(contents);
const typeSection = section(1, Uint8Array.from([1, /* func */ 0x60, 0, 0]));
const funcSection = section(3, Uint8Array.from([1, 0]));
const codeSection = section(10, concat([Uint8Array.from([1]), entry]));

const header = Uint8Array.from([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]);
const module = concat([header, typeSection, funcSection, codeSection]);

assertEq(WebAssembly.validate(module), false);
assertErrorMessage(() => new WebAssembly.Module(module), WebAssembly.CompileError,
                   /function body too big/);
