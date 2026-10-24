const { app, BrowserWindow, ipcMain, dialog, shell } = require('electron');
const path = require('path');
const c = require('./convert');

const wrapperDirs = [path.join(process.resourcesPath || '', 'wrapper'), path.join(__dirname, 'wrapper')];

app.whenReady().then(() => {
  const win = new BrowserWindow({
    width: 760, height: 720, title: 'VST3 to CLAP', backgroundColor: '#111318',
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true },
  });
  win.setMenuBarVisibility(false);
  win.loadFile('index.html');
});
app.on('window-all-closed', () => app.quit());

ipcMain.handle('scan', () => ({ plugins: c.scanVst3(), clapDir: c.clapFolder() }));
ipcMain.handle('browse', async () => {
  const r = await dialog.showOpenDialog({ title: 'Choose a .vst3 plug-in', properties: ['openFile', 'openDirectory'],
    filters: [{ name: 'VST3', extensions: ['vst3'] }] });
  return r.canceled ? null : r.filePaths[0];
});
ipcMain.handle('convert', (_e, plugins) => {
  let wrapper;
  try { wrapper = c.findWrapper(wrapperDirs); } catch (e) { return plugins.map((p) => ({ name: p.name, ok: false, error: e.message })); }
  return plugins.map((p) => {
    try { return { name: p.name, ok: true, out: c.convert({ vst3: p.path, wrapper, outDir: c.clapFolder() }) }; }
    catch (e) { return { name: p.name, ok: false, error: e.message }; }
  });
});
ipcMain.handle('reveal', (_e, p) => shell.showItemInFolder(p));
