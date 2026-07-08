// |jit-test| skip-if: !wasmComponentsEnabled(); local-include:canon-instantiate-harness.js

// canon lower: composite component value types (list, enum, flags, record,
// tuple, option), plus the >MAX_FLAT_PARAMS memory-spill path, through real
// instantiation.

function writeI32s(memory, realloc, values) {
  const ptr = realloc(0, 0, 4, values.length * 4);
  const view = new DataView(memory.buffer);
  values.forEach((v, i) => view.setInt32(ptr + i * 4, v, true));
  return ptr;
}

// list<s32>: param fits MAX_FLAT_PARAMS; result spills to a pointer.
{
  let seenArg;
  const { caller, memory, realloc } = lowerThrough(
    `(param "v" (list s32)) (result (list s32))`,
    (list) => { seenArg = list; return list.map((v) => v * 2); },
    `(import "env" "f" (func $f (param i32 i32 i32)))
     (export "f" (func $f))`);

  const ptr = writeI32s(memory, realloc, [1, 2, 3]);
  const retPtr = realloc(0, 0, 4, 8);
  caller.f(ptr, 3, retPtr);
  assertEq(JSON.stringify(seenArg), JSON.stringify([1, 2, 3]));
  const view = new DataView(memory.buffer);
  const outPtr = view.getInt32(retPtr, true);
  const outLen = view.getInt32(retPtr + 4, true);
  const out = [];
  for (let i = 0; i < outLen; i++) out.push(view.getInt32(outPtr + i * 4, true));
  assertEq(JSON.stringify(out), JSON.stringify([2, 4, 6]));
}

// enum: flattens to a single i32 discriminant, so neither direction spills.
{
  let seenArg;
  const { caller } = lowerThrough(
    `(param "v" (enum "a" "b" "c")) (result (enum "a" "b" "c"))`,
    (v) => { seenArg = v; return v; },
    `(import "env" "f" (func $f (param i32) (result i32)))
     (export "f" (func $f))`);
  assertEq(caller.f(1), 1);
  assertEq(seenArg, "b");
  assertEq(caller.f(0), 0);
  assertEq(seenArg, "a");
}

// flags: flattens to a single i32 bitset, so neither direction spills.
{
  let seenArg;
  const { caller } = lowerThrough(
    `(param "v" (flags "r" "w" "x" "extra1" "extra2"))
     (result (flags "r" "w" "x" "extra1" "extra2"))`,
    (v) => { seenArg = v; return v; },
    `(import "env" "f" (func $f (param i32) (result i32)))
     (export "f" (func $f))`);
  // r=1, w=0, x=1, extra1=0, extra2=1 -> bits 0,2,4 set = 0b10101 = 21
  assertEq(caller.f(0b10101), 0b10101);
  assertEq(JSON.stringify(seenArg),
    JSON.stringify({ r: true, w: false, x: true, extra1: false, extra2: true }));
}

// record{a:s32, b:f64}: params are individual scalars; result spills to a
// pointer.
{
  let seenArg;
  const { caller, memory, realloc } = lowerThrough(
    `(param "a" s32) (param "b" f64)
     (result (record (field "a" s32) (field "b" f64)))`,
    (a, b) => { seenArg = [a, b]; return { a: a + 1, b: b + 1.5 }; },
    `(import "env" "f" (func $f (param i32 f64 i32)))
     (export "f" (func $f))`);
  const retPtr = realloc(0, 0, 8, 16);
  caller.f(7, 3.5, retPtr);
  assertEq(JSON.stringify(seenArg), JSON.stringify([7, 3.5]));
  const view = new DataView(memory.buffer);
  assertEq(view.getInt32(retPtr, true), 8);
  assertEq(view.getFloat64(retPtr + 8, true), 5);
}

// record{a:s32}: a single scalar field flattens to exactly one core value
// (<= MAX_FLAT_RESULTS), so - unlike the record{a,b} case above - the
// result does *not* spill to a pointer; it comes back as the field's own
// flat i32 value directly.
{
  const { caller } = lowerThrough(
    `(result (record (field "a" s32)))`,
    () => ({ a: 42 }),
    `(import "env" "f" (func $f (result i32)))
     (export "f" (func $f))`);
  assertEq(caller.f(), 42);
}

// tuple<s32,s32>: params are individual scalars; the tuple-shaped result
// spills to a pointer.
{
  let seenArg;
  const { caller, memory, realloc } = lowerThrough(
    `(param "a" s32) (param "b" s32) (result (tuple s32 s32))`,
    (a, b) => { seenArg = [a, b]; return [b, a]; },
    `(import "env" "f" (func $f (param i32 i32 i32)))
     (export "f" (func $f))`);
  const retPtr = realloc(0, 0, 4, 8);
  caller.f(10, 20, retPtr);
  assertEq(JSON.stringify(seenArg), JSON.stringify([10, 20]));
  const view = new DataView(memory.buffer);
  assertEq(view.getInt32(retPtr, true), 20);
  assertEq(view.getInt32(retPtr + 4, true), 10);
}

// option<s32>: params are (disc, payload); result spills to a pointer to
// {disc@0 u8, payload@4 i32}.
{
  let seenArg;
  const { caller, memory, realloc } = lowerThrough(
    `(param "v" (option s32)) (result (option s32))`,
    (v) => { seenArg = v; return v === null ? null : v * 2; },
    `(import "env" "f" (func $f (param i32 i32 i32)))
     (export "f" (func $f))`);

  let retPtr = realloc(0, 0, 4, 8);
  caller.f(1, 42, retPtr);
  assertEq(seenArg, 42);
  let view = new DataView(memory.buffer);
  assertEq(view.getUint8(retPtr), 1);
  assertEq(view.getInt32(retPtr + 4, true), 84);

  retPtr = realloc(0, 0, 4, 8);
  caller.f(0, 0, retPtr);
  assertEq(seenArg, null);
  view = new DataView(memory.buffer);
  assertEq(view.getUint8(retPtr), 0);
}

// >MAX_FLAT_PARAMS(17) params: spills to a single pointer param.
{
  let seenArgs;
  const params17 = Array.from({ length: 17 }, (_, i) => `(param "p${i}" s32)`).join(" ");
  const { caller, memory, realloc } = lowerThrough(
    `${params17} (result s32)`,
    (...args) => { seenArgs = args; return args.reduce((a, b) => a + b, 0); },
    `(import "env" "f" (func $f (param i32) (result i32)))
     (export "f" (func $f))`);

  const values = [];
  for (let i = 0; i < 17; i++) values.push(i + 1);
  const ptr = writeI32s(memory, realloc, values);
  const expected = values.reduce((a, b) => a + b, 0);
  assertEq(caller.f(ptr), expected);
  assertEq(JSON.stringify(seenArgs), JSON.stringify(values));
}

// Nested composite: a zero-param function returning a {name, values} object
// for a record{name: string, values: list<s32>} result.
{
  const { caller, memory, realloc } = lowerThrough(
    `(result (record (field "name" string) (field "values" (list s32))))`,
    () => ({ name: "xy", values: [4, 5, 6] }),
    `(import "env" "f" (func $f (param i32)))
     (export "f" (func $f))`);

  const retPtr = realloc(0, 0, 4, 16);
  caller.f(retPtr);
  const view = new DataView(memory.buffer);
  const namePtr = view.getInt32(retPtr, true);
  const nameLen = view.getInt32(retPtr + 4, true);
  const valuesPtr = view.getInt32(retPtr + 8, true);
  const valuesLen = view.getInt32(retPtr + 12, true);
  const nameBytes = new Uint8Array(memory.buffer, namePtr, nameLen);
  assertEq(String.fromCharCode(...nameBytes), "xy");
  const values = [];
  for (let i = 0; i < valuesLen; i++) values.push(view.getInt32(valuesPtr + i * 4, true));
  assertEq(JSON.stringify(values), JSON.stringify([4, 5, 6]));
}
