// |jit-test| skip-if: !wasmComponentsEnabled()

// The rest of ComponentError is covered by the WPT tests in
// wasm/component/jsapi/interface.any.js.

const { ComponentError } = WebAssembly;

// `data` is not an options object, so `cause` is never read from it.
let data = { get cause() { throw new Error("cause read"); } };
let error = new ComponentError("message", data);
assertEq(error.data, data);
assertEq(Object.hasOwn(error, "cause"), false);

// `data` and the argument after it are not the non-standard fileName and
// lineNumber arguments.
error = new ComponentError("message", "data", 42);
assertEq(error.data, "data");
assertEq(error.fileName.endsWith("component-error.js"), true);
assertEq(error.lineNumber, 16);

assertEq(Error.isError(error), true);
