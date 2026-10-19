// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// The start function runs during instantiation, after every import has been
// read and before guest resource classes and the exports object are created.

for_each_entry_point(async (t, instantiate) => {
  const calls = [];
  const instance = await instantiate("instance/start", { recv(x) { calls.push(x); } });
  assert_array_equals(calls, [7], "recv was called once by start");
  assert_equals(instance.exports.answer(), 42);
  assert_array_equals(calls, [7], "calling an export does not run start again");
}, "start: runs once during instantiation");

for_each_entry_point(async (t, instantiate) => {
  const log = [];
  const imports = loggingProxy({ recv(x) { log.push(`recv ${x}`); } }, log);
  await instantiate("instance/start", imports);
  assert_array_equals(log, ["get recv", "recv 7"]);
}, "start: every import is read before start runs");

for_each_entry_point(async (t, instantiate) => {
  const calls = [];
  const thisValues = [];
  await instantiate("instance/start", { recv(x) { calls.push(x); thisValues.push(this); } });
  assert_array_equals(calls, [7]);
  assert_array_equals(thisValues, [undefined], "a plain function import is called with undefined this");
}, "start: import called from start receives undefined as this");

for_each_entry_point(async (t, instantiate) => {
  const calls = [];
  await promise_rejects_js(t, WebAssembly.RuntimeError,
                           (async () => instantiate("instance/start-trap", { recv(x) { calls.push(x); } }))());
  assert_array_equals(calls, [7], "recv ran before the trap");
}, "start: a trap fails instantiation with RuntimeError after side effects");

for_each_entry_point(async (t, instantiate) => {
  const calls = [];
  await promise_rejects_js(t, TypeError,
                           (async () => instantiate("instance/start-then-type-error",
                                                    { recv(x) { calls.push(x); }, R: class {} }))());
  assert_array_equals(calls, [7], "start ran before the exports were checked");
}, "start: runs before a public re-export of a host resource type fails instantiation with TypeError");

for_each_entry_point(async (t, instantiate) => {
  const error = new Error("from recv");
  let thrown;
  try {
    await instantiate("instance/start", { recv() { throw error; } });
  } catch (e) {
    thrown = e;
  }
  assert_equals(Object.getPrototypeOf(thrown), WebAssembly.RuntimeError.prototype,
                "a throwing import with no result type traps");
  assert_not_equals(thrown, error, "the original exception is not propagated");
}, "start: an import throwing during start traps");

for_each_entry_point(async (t, instantiate) => {
  const calls = [];
  const a = await instantiate("instance/start", { recv(x) { calls.push(`a ${x}`); } });
  const b = await instantiate("instance/start", { recv(x) { calls.push(`b ${x}`); } });
  assert_array_equals(calls, ["a 7", "b 7"]);
  assert_not_equals(a, b);
}, "start: runs once per instantiation");
