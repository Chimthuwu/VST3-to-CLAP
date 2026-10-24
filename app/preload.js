const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('api', {
  scan: () => ipcRenderer.invoke('scan'),
  browse: () => ipcRenderer.invoke('browse'),
  convert: (p) => ipcRenderer.invoke('convert', p),
  reveal: (p) => ipcRenderer.invoke('reveal', p),
});
