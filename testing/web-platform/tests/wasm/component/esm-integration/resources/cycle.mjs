import { run } from "./logger.wasm";
// Not a hoisted function declaration, so the binding is in its temporal dead
// zone until this module evaluates.
const log = () => {};
export default log;
export const result = run();
