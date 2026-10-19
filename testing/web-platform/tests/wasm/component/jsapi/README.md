# Component JS-API tests

Web-platform-tests for the
[component JS-API](https://github.com/WebAssembly/component-model/blob/main/design/mvp/JS-Reference.md).
`../esm-integration` holds the ESM-integration tests.

## Building

Fixtures are hand-written `.wat` files under `components/`, one per test
concern, compiled to a sibling `.wasm`. Both are checked in, so the tests run
without any tooling. After editing a `.wat`, rebuild with:

```
python3 ../tools/build.py
```

`build.py` runs `wasm-tools parse` then `wasm-tools validate --features all`
on every fixture here and in `../esm-integration/resources`. Point it at a specific binary with `--wasm-tools PATH` or
`WASM_TOOLS=PATH`.

The fixtures use the strict alias syntax (`(memory (core memory $m "mem"))`)
that wasm-tools 1.255 introduced. Released wasm-tools also still rejects
`[get]`/`[set]` names and `external-id`. Until
[wasm-tools#2660](https://github.com/bytecodealliance/wasm-tools/pull/2660)
ships, build that branch and use it:

```
git clone https://github.com/bytecodealliance/wasm-tools ~/src/wasm-tools
cd ~/src/wasm-tools && gh pr checkout 2660 && cargo build
WASM_TOOLS=~/src/wasm-tools/target/debug/wasm-tools python3 ../tools/build.py
```

## Running

The `.any.js` tests run in `window`, `dedicatedworker` and `jsshell`. In
mozilla-central, run them in the browser with:

```
./mach wpt testing/web-platform/tests/wasm/component/
```

or in a SpiderMonkey shell build with:

```
./mach jstests --wpt=enabled testing/web-platform/tests/wasm/component/
```

Tests fetch their fixtures. In the shell, `fetch` is a shim from
`js/src/tests/web-platform-test-shims.js` that reads files from the WPT tree.
The `.html` tests, including everything in `../esm-integration`, only run in
the browser.

## Conventions

- Every `.any.js` test starts with
  `// META: global=window,dedicatedworker,jsshell`.
- Every test is a `promise_test` and includes the harness with
  `// META: script=/wasm/component/jsapi/harness/component-helpers.js` and
  `// META: script=/wasm/component/jsapi/harness/assertions.js`.
- Fixtures live at `components/<area>/<test-stem>[-<variant>].wat` and are
  loaded with `componentBytes("<area>/<test-stem>")` or
  `instantiateComponent(name, imports, options)`.
- Anything that must hold for all three instantiation paths uses
  `for_each_entry_point` or `test_instantiation_error`.
- Subtest names quote the behaviour under test in the spec's words.
- Passthrough imports are named `echo`, spy imports `recv`, and exports
  `roundtrip`, `send`, `take` respectively. Guest resources use the label
  `counter` unless the test is about names.

## Wat recipes

**Passthrough** exercises both conversions in both directions with one
component. `roundtrip(x: T) -> T` lowers `x`, calls the import
`echo(x: T) -> T`, and lifts what it returns. The JS import asserts what it
received and picks what to return.

```wat
(component
  (import "echo" (func $echo (param "x" u32) (result u32)))
  (core func $echo' (canon lower (func $echo)))
  (core module $M
    (import "" "echo" (func $echo (param i32) (result i32)))
    (func (export "roundtrip") (param i32) (result i32)
      (call $echo (local.get 0))))
  (core instance $m (instantiate $M (with "" (instance (export "echo" (func $echo'))))))
  (func $roundtrip (param "x" u32) (result u32) (canon lift (core func $m "roundtrip")))
  (export "roundtrip" (func $roundtrip))
)
```

Types that need linear memory add a `$Memory` core module exporting `mem`
and a bump `realloc`, lower the import with
`(memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))`,
and lift the export the same way. See `components/values/string.wat` for the
full shape, including the result pointer that a multi-value result needs.

Every `record`, `variant`, `enum` and `flags` type that appears in an imported
or exported function type must itself be imported or exported as a named type
in the same scope, and the function must refer to that alias:

```wat
(type $point (record (field "x" u32) (field "y" u32)))
(import "point" (type $point' (eq $point)))   ;; for imported functions
(export $point'' "point" (type $point))        ;; for exported functions
```

Validation rejects the function otherwise. A single type import serves both
sides, which is what most fixtures do.

**Spy** checks `ToJSValue` for values the guest produces. `send()` calls the
import `recv(x: T)` with a constant built in the core module, either as
immediates or as canonical-ABI bytes in a data segment. Guest resource values
come from `resource.new`.

**Sink** checks `ToComponentValue` on its own. `take(x: T) -> u32` returns
something derived from the lowered value, such as the first flat value or a
byte read from memory.

**Counter resource** is the standard guest resource: `(resource (rep i32))`
with a destructor that bumps a counter exposed through a `drop-count` export,
so destruction is observable from JS. Because the destructor lives in a core
module that also needs `resource.new`, route the destructor through a
`funcref` table in a small helper module, as `test/resources/borrows.wast`
does.

### Flattening

The core function a `canon lift`/`canon lower` expects follows the canonical
ABI. Get it wrong and `build.py` fails validation, which is the point.

| Component type | Flat core types |
|---|---|
| `bool`, `s8`..`u32`, `char`, `enum`, `flags` (≤32 labels), `own`, `borrow` | `i32` |
| `s64`, `u64` | `i64` |
| `f32` / `f64` | `f32` / `f64` |
| `string`, `list<T>`, `map<K, V>` | `i32 i32` (pointer, length) |
| `list<T, N>` | `N` copies of `T`'s flat types |
| `record`, `tuple` | the fields' flat types, concatenated |
| `option<T>`, `variant`, `result` | `i32` discriminant, then the join of every case's flat types |

The join of two flat types is that type if they are equal, `i32` for
`i32`/`f32`, and `i64` otherwise. More than 16 flat parameters are passed as
a single `i32` pointer. More than one flat result is returned through a
result pointer, which is an extra trailing `i32` parameter on a lowered
import and an `i32` result on a lifted export.

## Decisions baked into the tests

These resolve gaps the spec text left open. Change the tests if the spec goes
another way.

- `ComponentError` requires `new`; calling it throws a `TypeError`.
- A trap locks the instance down: every later call into it throws
  `WebAssembly.RuntimeError`.
- A type import that is not a resource is skipped without reading the
  imports object, and a type export that is not a resource is left off the
  exports object. Named value types have to be imported or exported for
  validation to accept the functions that use them, so rejecting them would
  reject every component with a `record` in its API.
- `compileStreaming` and `instantiateStreaming` are out of scope.
- Objects and errors created by a conversion belong to the component realm.
- `ToComponentValueList` consumes the whole iterable before converting any
  element, so that the canonical ABI can allocate the list up front. A failing
  element therefore locks down the instance.
- A transparent `(eq)` type import is not read from the imports object.
- The brand check for a guest class used as a host type import looks only at
  the class, so a disposed instance still passes.
- `Symbol.dispose` on a guest resource class is named `[Symbol.dispose]`.
- A `[method]` or `[static]` export with a reserved class member name
  (`constructor`, `prototype`, `name`, ...) is a `TypeError`, until the spec
  gives such members a JS name on the exports object.
- The `Symbol.dispose` of a host resource is read from its constructor's
  `prototype`, and an option is "nested" only when it is the payload of an
  option.
- Each export declaration gets its own function object, even when two
  declarations name the same function.
- ESM: `[get]`/`[set]` exports have no namespace binding, a named import
  with no matching export is a `SyntaxError`, and a cycle is a
  `ReferenceError`.
- Conversion is interleaved with canonical ABI lowering, so `Get`s happen in
  lowering order. A conversion failure after the call has entered the guest
  (ran `realloc` or touched a handle table) locks the instance down like a
  trap, while the original exception still propagates. A failure before that
  leaves the instance untouched.
