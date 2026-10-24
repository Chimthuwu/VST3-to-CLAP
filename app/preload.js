const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('api', {
  scan: () => ipcRenderer.invoke('scan'),
  browse: () => ipcRenderer.invoke('browse'),
  convert: (p) => ipcRenderer.invoke('convert', p),
  setDir: () => ipcRenderer.invoke('setDir'),
  reveal: (p) => ipcRenderer.invoke('reveal', p),
});
