// |jit-test| skip-if: !wasmComponentsEnabled(); local-include:canon-instantiate-harness.js

// canon lower over scalar component value types, through real instantiation.
//
// Note: explicit WebIDLOperationType overrides (EnforceRange/Clamp coercion,
// variadic/default-filling behavior, result-presence mismatches) only exist
// as testing-only surface on the removed ComponentCanonLower built-in - a
// plain JS import has no WebIDL type of its own, so real instantiation
// always uses the default `(any...) -> any` passthrough. Those cases have
// no migration target here.

function identityLowered(componentType, coreType) {
  let seenArg;
  const { caller } = lowerThrough(
    `(param "v" ${componentType}) (result ${componentType})`,
    (v) => { seenArg = v; return v; },
    `(import "env" "f" (func $f (param ${coreType}) (result ${coreType})))
     (export "f" (func $f))`);
  return [caller.f, () => seenArg];
}

{
  const [fn, seen] = identityLowered("bool", "i32");
  assertEq(fn(1), 1);
  assertEq(seen(), true);
  assertEq(fn(0), 0);
  assertEq(seen(), false);
}
{
  const [fn, seen] = identityLowered("s8", "i32");
  assertEq(fn(-42), -42);
  assertEq(seen(), -42);
}
{
  const [fn, seen] = identityLowered("u8", "i32");
  assertEq(fn(200), 200);
  assertEq(seen(), 200);
}
{
  const [fn, seen] = identityLowered("s16", "i32");
  assertEq(fn(-1234), -1234);
  assertEq(seen(), -1234);
}
{
  const [fn, seen] = identityLowered("u16", "i32");
  assertEq(fn(50000), 50000);
  assertEq(seen(), 50000);
}
{
  const [fn, seen] = identityLowered("s32", "i32");
  assertEq(fn(-70000), -70000);
  assertEq(seen(), -70000);
}
{
  const [fn, seen] = identityLowered("u32", "i32");
  assertEq(fn(70000), 70000);
  assertEq(seen(), 70000);
}
{
  // s64/u64: the raw core boundary uses BigInt, but the wrapped JS function
  // sees a regular Number (converted via ToNumber).
  const [fn, seen] = identityLowered("s64", "i64");
  assertEq(fn(-5n), -5n);
  assertEq(seen(), -5);
}
{
  const [fn, seen] = identityLowered("u64", "i64");
  assertEq(fn(5n), 5n);
  assertEq(seen(), 5);
}
{
  const [fn, seen] = identityLowered("f32", "f32");
  assertEq(fn(1.5), 1.5);
  assertEq(seen(), 1.5);
}
{
  const [fn, seen] = identityLowered("f64", "f64");
  assertEq(fn(2.5), 2.5);
  assertEq(seen(), 2.5);
}

// NaN/Infinity round-trip cleanly through the raw core f32/f64 boundary.
{
  const [fn] = identityLowered("f64", "f64");
  assertEq(Number.isNaN(fn(NaN)), true);
  assertEq(fn(Infinity), Infinity);
  assertEq(fn(-Infinity), -Infinity);
}
{
  const [fn] = identityLowered("f32", "f32");
  assertEq(Number.isNaN(fn(NaN)), true);
  assertEq(fn(Infinity), Infinity);
}

{
  const [fn, seen] = identityLowered("char", "i32");
  assertEq(fn(65), 65);
  assertEq(seen(), "A");
}

// Non-ASCII char: a codepoint outside ASCII, and one outside the BMP.
{
  const [fn, seen] = identityLowered("char", "i32");
  assertEq(fn(0x20AC), 0x20AC); // '€'
  assertEq(seen(), "€");
  assertEq(fn(0x1F600), 0x1F600); // outside the BMP
  assertEq(seen(), String.fromCodePoint(0x1F600));
}

// string: params are a (ptr, len) pair; the result spills to a pointer.
{
  let seenArg;
  const { caller, memory, realloc } = lowerThrough(
    `(param "v" string) (result string)`,
    (s) => { seenArg = s; return s; },
    `(import "env" "f" (func $f (param i32 i32 i32)))
     (export "f" (func $f))`);

  const bytes = [104, 101, 108, 108, 111]; // "hello"
  const ptr = realloc(0, 0, 1, bytes.length);
  new Uint8Array(memory.buffer, ptr, bytes.length).set(bytes);
  const retPtr = realloc(0, 0, 4, 8);

  caller.f(ptr, bytes.length, retPtr);
  assertEq(seenArg, "hello");
  const mem = new DataView(memory.buffer);
  const outPtr = mem.getInt32(retPtr, true);
  const outLen = mem.getInt32(retPtr + 4, true);
  const outBytes = new Uint8Array(memory.buffer, outPtr, outLen);
  assertEq(String.fromCharCode(...outBytes), "hello");
}

// Empty string round-trip.
{
  let seenArg;
  const { caller, memory, realloc } = lowerThrough(
    `(param "v" string) (result string)`,
    (s) => { seenArg = s; return s; },
    `(import "env" "f" (func $f (param i32 i32 i32)))
     (export "f" (func $f))`);
  const retPtr = realloc(0, 0, 4, 8);
  caller.f(0, 0, retPtr);
  assertEq(seenArg, "");
  const mem = new DataView(memory.buffer);
  assertEq(mem.getInt32(retPtr + 4, true), 0);
}

// No result: the JS callee's return value is simply discarded.
{
  let seenArg;
  const { caller } = lowerThrough(
    `(param "v" s32)`,
    (a) => { seenArg = a; },
    `(import "env" "f" (func $f (param i32)))
     (export "f" (func $f))`);
  caller.f(17);
  assertEq(seenArg, 17);
}
