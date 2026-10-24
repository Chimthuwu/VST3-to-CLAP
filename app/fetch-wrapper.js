// Downloads the wrapper for this OS from the latest GitHub Release into ./wrapper/.
const { execFileSync } = require('child_process');
const fs = require('fs'), path = require('path');
const plat = { win32: 'Windows-x64', darwin: 'macOS-universal', linux: 'Linux-x64' }[process.platform];
const tmp = fs.mkdtempSync(path.join(require('os').tmpdir(), 'v2c-'));
execFileSync('gh', ['release', 'download', '--repo', 'Chimthuwu/vst3-to-clap', '-p', `*${plat}*`, '-D', tmp], { stdio: 'inherit' });
const zip = path.join(tmp, fs.readdirSync(tmp).find((f) => f.endsWith('.zip')));
const x = path.join(tmp, 'x');
if (process.platform === 'win32') execFileSync('powershell', ['-NoProfile', '-Command', `Expand-Archive -Force '${zip}' '${x}'`]);
else execFileSync('unzip', ['-q', zip, '-d', x]);
const root = path.join(x, fs.readdirSync(x)[0]);
fs.mkdirSync('wrapper', { recursive: true });
fs.cpSync(path.join(root, 'vst3-to-clap-wrapper.clap'), path.join('wrapper', 'vst3-to-clap-wrapper.clap'), { recursive: true });
console.log('wrapper ready in ./wrapper');
