import { answer, Counter, math } from "./greeter.wasm";
postMessage([answer(), new Counter(3).get(), math.double(21)]);
