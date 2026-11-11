// Runs the analysis test program compiled to WebAssembly: node tests/wasi_run.mjs prog.wasm file.wav [columns]
// Lets the analysis tests run on machines where launching a fresh .exe is awkward.
import { readFileSync } from 'node:fs';
import { dirname, basename, resolve } from 'node:path';
import { WASI } from 'node:wasi';

process.removeAllListeners('warning');
const [prog, wav, ...rest] = process.argv.slice(2);
const dir = dirname(resolve(wav));
const wasi = new WASI({ version: 'preview1', args: ['analysis_test', '/d/' + basename(wav), ...rest], preopens: { '/d': dir } });
const mod = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(prog)), wasi.getImportObject());
process.exitCode = wasi.start(mod);
