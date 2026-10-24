// Verifies the JS patcher produces byte-identical output to the C++ converter.
const fs = require('fs'), assert = require('assert');
const c = require('./convert');
const [,, wrapper, vst3, ref] = process.argv;
if (!wrapper) { console.log('usage: node test-convert.js <wrapper.clap> <plugin.vst3> <reference.clap made by vst3-to-clap>'); process.exit(0); }
const out = c.convert({ vst3, wrapper, outDir: require('os').tmpdir(), name: 'v2c-test' });
assert(fs.readFileSync(out).equals(fs.readFileSync(ref)), 'output differs from C++ converter');
console.log('OK: identical to C++ converter output');
