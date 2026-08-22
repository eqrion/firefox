// Usage: js [--wasm-compiler=TIER] bench.js <validate|compile> <corpus-dir> [iterations] [warmup]
const mode = scriptArgs[0];
const dir = scriptArgs[1];
const ITER = parseInt(scriptArgs[2] || "8", 10);
const WARMUP = parseInt(scriptArgs[3] || "2", 10);

function median(values) {
	const s = values.slice().sort((a, b) => a - b);
	const mid = Math.floor(s.length / 2);
	return s.length % 2 ? s[mid] : (s[mid - 1] + s[mid]) / 2;
}

function run(bytes) {
	if (mode === "validate") {
		return WebAssembly.validate(bytes);
	}
	new WebAssembly.Module(bytes);
	return true;
}

const files = os.file.listDir(dir).filter((f) => f.endsWith(".wasm")).sort();
print("file,bytes,ok,median_ms,min_ms,max_ms,iterations,error");
for (const filename of files) {
	const filepath = os.path.join(dir, filename);
	const bytes = os.file.readFile(filepath, "binary");
	let ok = true;
	let error = "";
	const times = [];
	try {
		for (let i = 0; i < WARMUP; i++) ok = run(bytes);
		for (let i = 0; i < ITER; i++) {
			gc();
			const start = performance.now();
			ok = run(bytes);
			const end = performance.now();
			times.push(end - start);
		}
	} catch (e) {
		ok = false;
		error = String(e).replace(/[\r\n,]+/g, " ");
	}
	if (times.length) {
		print(`${filename},${bytes.byteLength},${ok},${median(times)},${Math.min(...times)},${Math.max(...times)},${times.length},`);
	} else {
		print(`${filename},${bytes.byteLength},${ok},,,,0,${error}`);
	}
}
