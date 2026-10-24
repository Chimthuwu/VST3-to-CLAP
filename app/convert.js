// Core conversion logic (no Electron deps so it can be tested with plain node).
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const MARKER = Buffer.from('@@VST3-TO-CLAP:EMBEDDED-CONFIG:v1@@');
const BLOCK = 4096;

function vst3Folders() {
  if (process.platform === 'win32') {
    return [path.join(process.env.COMMONPROGRAMFILES || 'C:\\Program Files\\Common Files', 'VST3'),
            path.join(process.env.LOCALAPPDATA || '', 'Programs', 'Common', 'VST3')];
  }
  if (process.platform === 'darwin') return ['/Library/Audio/Plug-Ins/VST3', path.join(os.homedir(), 'Library/Audio/Plug-Ins/VST3')];
  return ['/usr/lib/vst3', '/usr/local/lib/vst3', path.join(os.homedir(), '.vst3')];
}

function clapFolder() {
  if (process.platform === 'win32') return path.join(process.env.LOCALAPPDATA || os.homedir(), 'Programs', 'Common', 'CLAP');
  if (process.platform === 'darwin') return path.join(os.homedir(), 'Library/Audio/Plug-Ins/CLAP');
  return path.join(os.homedir(), '.clap');
}

// Finds every *.vst3 (file or bundle), descending into plain vendor sub-folders.
function scanVst3(dirs = vst3Folders(), depth = 2) {
  const found = [];
  const walk = (dir, d) => {
    let entries;
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
    for (const e of entries) {
      const p = path.join(dir, e.name);
      if (e.name.toLowerCase().endsWith('.vst3')) found.push({ name: e.name.replace(/\.vst3$/i, ''), path: p });
      else if (e.isDirectory() && d > 0) walk(p, d - 1);
    }
  };
  dirs.forEach((d) => walk(d, depth));
  return found.sort((a, b) => a.name.localeCompare(b.name));
}

function patch(buf, vst3Path, classFilter = '') {
  const payload = Buffer.concat([Buffer.from(vst3Path, 'utf8'), Buffer.from([0]), Buffer.from(classFilter, 'utf8'), Buffer.from([0])]);
  const cap = BLOCK - MARKER.length;
  if (payload.length > cap) throw new Error('That plug-in path is too long to embed.');
  let n = 0;
  for (let i = buf.indexOf(MARKER); i >= 0; i = buf.indexOf(MARKER, i + MARKER.length)) {
    const start = i + MARKER.length;
    buf.fill(0, start, start + cap);
    payload.copy(buf, start);
    n++;
  }
  if (!n) throw new Error('The bundled wrapper is not a valid vst3-to-clap build.');
}

function findWrapper(dirs) {
  const name = 'vst3-to-clap-wrapper.clap';
  for (const d of dirs) { const p = path.join(d, name); if (fs.existsSync(p)) return p; }
  throw new Error('Wrapper not found. Run "npm run fetch-wrapper" (or reinstall the app).');
}

function convert({ vst3, wrapper, outDir, name }) {
  if (!fs.existsSync(vst3)) throw new Error(`Can't find ${vst3}`);
  const outName = (name || path.basename(vst3).replace(/\.vst3$/i, '')).replace(/[<>:"/\\|?*]/g, '-');
  const out = path.join(outDir, `${outName}.clap`);
  fs.mkdirSync(outDir, { recursive: true });
  fs.rmSync(out, { recursive: true, force: true });
  if (fs.statSync(wrapper).isDirectory()) {            // macOS bundle
    fs.cpSync(wrapper, out, { recursive: true });
    const macos = path.join(out, 'Contents', 'MacOS');
    for (const f of fs.readdirSync(macos)) {
      const p = path.join(macos, f);
      const buf = fs.readFileSync(p);
      patch(buf, vst3);
      fs.writeFileSync(p, buf, { mode: 0o755 });
    }
    const plist = path.join(out, 'Contents', 'Info.plist');
    if (fs.existsSync(plist)) {
      const slug = outName.replace(/[^A-Za-z0-9]/g, '-');
      let t = fs.readFileSync(plist, 'utf8').replaceAll('org.vst3-to-clap.wrapper', `org.vst3-to-clap.${slug}`);
      t = t.replace(/(<key>CFBundleName<\/key>\s*<string>)[^<]*(<\/string>)/, `$1${outName}$2`);
      fs.writeFileSync(plist, t);
    }
    execFileSync('codesign', ['--force', '--sign', '-', out]);   // patching voids the signature
  } else {
    const buf = fs.readFileSync(wrapper);
    patch(buf, vst3);
    fs.writeFileSync(out, buf);
  }
  return out;
}

module.exports = { scanVst3, convert, findWrapper, clapFolder, vst3Folders, patch, MARKER };
