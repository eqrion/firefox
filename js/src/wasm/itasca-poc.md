# itasca as SpiderMonkey's wasm validator

A prototype in which SpiderMonkey does not decode or validate wasm: itasca
does, and SpiderMonkey compiles what it reports. Thirteen commits on this
branch, three in `~/src/itasca`.

State: the whole wasm jit-test suite passes on both compilers and through both
tiers. `just difftest` agrees with itasca on all 2419 modules of its extracted
W3C corpus. `mach jsapi-tests wasm` passes.

## 1. Structure

### The boundary

`WasmItasca.{h,cpp}` is everything that touches the C ABI: owned-handle
deleters, error reporting, the implementation caps itasca does not carry, and
the four entry points the engine drives. `WasmItascaModule.h` is separate only
to break an include cycle with `WasmMetadata.h`.

The C ABI itself -- `js/src/wasm/itasca.h` and the `itasca_glue` crate
(`js/src/wasm/itasca_glue`) -- moved into this tree; it used to live in the
out-of-tree crate as `include/itasca.h` and `src/ffi/`. The header is
tracked here now rather than a gitignored symlink. `jsrust_shared` depends on
`itasca_glue`, with `extern crate itasca_glue;` so the `#[no_mangle]`
symbols survive the staticlib link; `itasca_glue` in turn is a Cargo path
dependency of the out-of-tree `itasca` crate itself, which owns only the
verified core and a public Rust API now.

### Who reads the bytes

| Phase | Before | Now |
| --- | --- | --- |
| `WebAssembly.validate` | `ValidateModule` | `ValidateWithItasca` |
| Module environment | `DecodeModuleEnvironment` | `DecodeEnvWithItasca`, translating `VwEnv` into `CodeMetadata`/`ModuleMetadata` |
| Code section walk | `Decoder::readVarU32` per entry | `DecodeCodeSectionWithItasca`, driving `itasca_validate_code_with` |
| Function bodies | `OpIter` pull loop per compiler | `DriveBodyWithItasca`, hooks per operator |
| Module tail | `DecodeModuleTail` | `DecodeTailWithItasca` |

The code-section partition is not trusted, and is no longer hand-rolled either:
`itasca_validate_code_with` frames each entry from its own size prefix and
hands it over unvalidated — which is what lets it go to a compile thread before
the entries ahead of it are validated — and shows the entries tile the section
exactly before it returns. `code_entry_extent_frames` is the equality that
check gives which the old hand-rolled partition never had.

### Three regions, not one buffer

A module is not always one buffer: a streamed one arrives as three separate
allocations, and itasca's positions index the bytes it is handed. So each
phase is driven over its own region with a region-relative position, and a
`BytecodeRegion` carries the region's module offset for re-basing what itasca
reports. Every path does this, buffered or streamed, so there is one way to
drive a module rather than one per caller.

The split moved with it: a bytecode source is split at the code section's id
byte rather than at its contents, so the section's framing is inside the region
itasca reads. `decode_env` reports the code section's header only after
reading it, which an environment region that stopped at the contents cannot
hold; and the section's size prefix would otherwise be in a region no validator
frames it against. The two ends have to agree, so a section between where the
environment stopped and where the split is rejects the module.

Streaming falls out of this: the code section's region is walked as it arrives,
with each read waiting for its own bytes and each entry handed to a compile
thread as soon as it is whole. `itasca_status_needs_more_input` is still
unused, because a wait is bounded by what the next read can need rather than
found by asking itasca whether it has enough. A stream that ends inside the
code section is a rejection this side of the boundary reports: the bytes never
reached itasca, so it has nothing to say about them.

### The compiler inversion

itasca owns the decode-and-validate loop and reports each accepted operator.
Both compilers were pull-based; both are now driven, and neither uses `OpIter`
any more.

Each compiler owns its own value and control stack directly — for Ion the
value stack *is* the `MDefinition*` operand stack, same as before, just no
longer borrowed from `OpIter`. `push`/`pop`/`peekN`/`pokeN` and a small
`ControlFrame` (label kind, block type, the stack height a branch resets to,
and whether the block is already polymorphic) replace `OpIter`'s value and
control stacks and its type checks: the type checks are gone outright, not
kept as a cross-check, since itasca already proved them and a second
validator disagreeing would be a bug in one of them, not something to paper
over at runtime.

Each hook calls one emitter method directly; there is no switch behind it.
`WasmItascaOps.h` maps each hook name to its opcode (`F(on_i32_add,
I32Add)`), shared by both compilers, so `on_i32_add` calls `emitI32Add` and the
two cannot drift. A hook needing more than its emitter's stack effect — a
memory op's alignment and offset, a branch's target block type — gets it as a
plain argument, since itasca already decoded it.

Both compilers build their locals from `on_function_start`'s `VwContext`,
which is also where the loop reports how many there are and their types.
Baseline's `BaseCompiler` exists before its locals are known and gets them
through `init()`; Ion's `RootCompiler`/`FunctionCompiler` do the same
(`CompileInfo::setWasmNumLocals` fixes up the slot count `CompileInfo`
computed against zero). Neither is behind a `Maybe`, and neither reads
`itasca_code_entry_locals` any more — `EntryLocalsWithItasca` is deleted.
The crate's `itasca_code_entry_locals`/`itasca_locals_*` FFI is now dead
code on the SpiderMonkey side; retiring it from the crate itself is a
separate, later cleanup.

### Tier-2

Recompiling a body means driving itasca over it again, so `ItascaModule`
retains the environment and the code section on `CodeTailMetadata`, with each
entry's offset. Validating one entry reads nothing outside it, so a later tier
points at it within the section, which is the same region-relative drive tier-1
does and the same base to add. Trap sites want module offsets.

### itasca-side additions

The extraction's own additions — `code_entry_extent` for partitioning the code
section without validating it, `code_entry_locals` for a compiler that exists
before its body starts, and `custom_sections_from` for a custom-section walk
starting where the caller says — were each a sign of surface the crate did not
have yet rather than a design SpiderMonkey wanted to keep. A later crate change
replaced them with a hooks API instead of keeping them alongside it:
`VwCodeVisitor` (`on_need_bytes`, `on_code_entry`) drives the code section in
one call, framing and validating-obligation and all, so there is no second,
unvalidated walk on this side to keep in step with the compile path; and
`VwModuleVisitor::on_custom_section` reports a custom section as
`decode_env_with`/`decode_tail_with` passes it, rather than a dedicated walk
after the fact. `code_entry_locals` was already dead code here before that
change; retiring it and `custom_sections`/`custom_sections_from` from the crate
is a separate, later cleanup.

### Test infrastructure

- The spec harness now asserts `WebAssembly.validate` agrees with
  `WebAssembly.Module`. It previously called neither together, so none of the 70
  spec files was testing `validate` at all.
- `wasmTextToBinary` emitted the bulk-memory flags-2 elem encoding for every
  active segment. Fixed in `js/src/rust/wasm.rs`; left alone it breaks 22 spec
  files and a dozen jit-test sites the moment itasca is the decoder.
- `just difftest` runs `validate` over itasca's extracted corpus. It does not
  go through the text-to-binary compiler, so it isolates the FFI from encoder
  problems like that one.
- `just widen-messages` automates the error-message churn. Review its diff.

## 2. Made worse

### Excluded features

Everything that built its own module rather than parsing one, because there is
nothing for itasca to have validated. All post-1.0 except inlining.

| Feature | How |
| --- | --- |
| JS-PI, component model, SIMD | off at configure time (mozconfig) |
| Builtin modules, `WebAssembly.Function` | report an error |
| Ion inlining | `InliningHeuristics` declines every candidate |
| `wasmDumpIon` | reports an error; two tests skipped |

### Quality regressions

- **Baseline no longer folds a comparison into a following branch or select.**
  It did that by peeking at the next operator; a pushed dispatch reports one at
  a time.
- **Error messages are coarser.** itasca names the rule that was broken where
  SpiderMonkey named the operands, so one phrasing stands in for several:
  `stack mismatch` covers unused values at the end of a block, an `if` without
  an `else` whose parameters do not match its result, and a block that ends
  short. Module-level errors also carry no byte offset, by a deliberate choice
  in itasca. The widened jit-test regexes discriminate less finely than what
  they replaced.

### Local build state

`--enable-warnings-as-errors` is commented out of the mozconfig. It was
disabled for 175 dead SIMD and sign-extension helpers in
`WasmBaselineCompile.cpp`; disabling SIMD should have removed those, so it can
probably go back.

## 3. Profiling and benchmarking

### Build for it

Use `mozconfig.release-lto` (release, `--enable-optimize`, `--enable-lto=cross,thin`)
for any timing comparison. The debug mozconfig isn't representative, and
cross-language LTO changes inlining decisions enough that even a plain
release build (no LTO) can mislead.

```
MOZCONFIG=$(pwd)/mozconfig.release-lto ./mach build
```

**Stale-binary trap:** when only the out-of-tree `~/src/itasca` crate
changes, `mach build` sometimes reports success without relinking
`dist/bin/js` at all, so a build you expect to change the binary can silently
leave the previous one in place. Symptom: two builds that should differ hash
identically, or a fix appears to have the wrong sign. Always hash-check
before trusting a comparison:

```
shasum -a 256 obj-release-lto/dist/bin/js
```

Force a clean relink when in doubt:

```
rm -rf obj-release-lto/aarch64-apple-darwin/release/.fingerprint/itasca-*
rm -rf obj-release-lto/aarch64-apple-darwin/release/.fingerprint/itasca_glue-*
rm -f obj-release-lto/dist/bin/js obj-release-lto/aarch64-apple-darwin/release/libjsrust.a
find obj-release-lto/aarch64-apple-darwin/release/deps -iname '*itasca*' -delete
MOZCONFIG=$(pwd)/mozconfig.release-lto ./mach build
```

Run `mach build` in the foreground and wait for it. Starting a second build
before the first one's link step finishes races both against the same object
directory; if that happens, `pkill -f obj-release-lto` and one clean rebuild
recovers.

### Before/after on the crate

`~/src/itasca` is its own git repository, not part of this worktree, so a
plain `git stash` there is safe:

```
cd ~/src/itasca
git stash push -m 'change under test'   # rebuild, save the binary as "before"
git stash pop                           # rebuild, save the binary as "after"
```

### Corpus benchmarks

`tools/itasca/bench.py` drives `tools/itasca/bench.js` in the `js` shell
over a directory of `.wasm` files (default `~/src/wasm-corpus`) for
`validate`, `baseline`, and `ion`, and reports a geomean ratio per mode:

```
python3 tools/itasca/bench.py before/js after/js --iterations 10 --warmup 3 --details
```

Each binary staged outside the object directory needs `libmozglue.dylib`
copied alongside it (an rpath-relative dependency).

To measure the gap against unmodified mozilla-central rather than a previous
point on this branch, build the exact commit this branch forked from
(`git merge-base HEAD main`) with the same mozconfig; a drifted base commit
or a different mozconfig makes the comparison meaningless before any code is
even looked at.

### Profiling

`samply` plus a small driver script isolates the compile loop from shell
startup:

```js
// loop-compile.js
const bytes = os.file.readFile(scriptArgs[0], 'binary');
for (let i = 0; i < (parseInt(scriptArgs[1]) || 40); i++) new WebAssembly.Module(bytes);
```

```
DYLD_LIBRARY_PATH=<dir> samply record --save-only -o profile.json.gz -- \
  <dir>/js --wasm-compiler=baseline loop-compile.js <module>.wasm 30
```

Load the result with the `profiler-analysis` skill
(`profiler-cli load profile.json.gz --session name`). It usually can't
symbolicate a locally-built, unstripped shell, so `thread samples` shows
`js!0x...` instead of names; resolve those by hand:

```
atos -o <dir>/js -l 0x100000000 0x1<the offset, as nine hex digits>
```

(`0x100000000` is this build's fixed load address). For a dispatch-shape
question `atos` can't answer, i.e. jump table versus compare chain,
disassemble around the resolved symbol:

```
nm <dir>/js | grep <symbol substring>
llvm-objdump -d --start-address=<sym-addr> --stop-address=<sym-addr+N> <dir>/js
```

`adr` + `ldrh` (table load) + `add` + `br x9` is a real jump table; a run of
`cmp`/`b.eq`/`b.hi` is a compare chain. `llvm-symbolizer -i --obj=<dir>/js
<addr>` can give file:line, but this build's debug info generally doesn't
carry enough for heavily-inlined Rust, so don't rely on it.

Always compare against a mainline profile of the same workload before calling
a hot function itasca-specific: some of what shows up (`BaseCompiler::emitLoad`,
`vixl::MacroAssembler`, ...) is codegen work mainline pays too, just a bigger
share of the total once itasca's own overhead shrinks.

### Noise

Repeated back-to-back rebuilds and profiling runs heat up a laptop enough to
bias later runs against earlier ones. A single before/after comparison inside
one process is far more trustworthy than two runs minutes apart. When a
result looks surprising, rerun with the argument order swapped: a real effect
survives the swap, a thermal or ordering artifact flips or disappears.

## 4. Follow-ups

Roughly in order of value.

1. **Re-enable `--enable-warnings-as-errors`** and fix what falls out. Cheap,
   and it was only off for a reason that no longer applies.
2. **Give baseline one operator of lookahead.** A driver that holds the pending
   operator back as a delay line, emitting it when the next hook arrives,
   restores the comparison fold. Contained in the driver.
3. **Restore inlining.** Needs the retained environment plus each callee's entry
   offset reachable at inline time. `CodeTailMetadata` now has both, so this may
   be close to free.
4. **Error offsets for module-level rejections.** itasca's `DEVELOPMENT.md`
   prices this: threading a position through would mean replacing ~312 `?`s.
   Probably not worth it, but the jit-tests lost real detail.
5. **Retire the crate surface the hooks API replaced.** `code_entry_extent`,
   `code_entry_locals`/`VwLocals`, and `custom_sections`/`custom_sections_from`
   have no consumer here any more. `itasca_code_section` and
   `itasca_status_needs_more_input` are close behind it: nothing here calls
   the former, and `on_need_bytes` makes the latter's question moot by saying
   exactly which byte the walk wants. Removing them from the crate is a
   separate change from this one; `TRUST.md` now carries the theorems
   (`code_entry_extent_frames`, `validate_code_driven`, `validate_module_driven`)
   that were the open question here before the hooks API existed.
6. **Switch `ModuleMetadata`/`CodeMetadata` construction to read itasca's
   accessors directly**, instead of copying out of a validated `VwEnv`.
   Separate from and larger than the compiler inversion above.
