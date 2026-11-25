const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('ariel', {
  build: (values) => ipcRenderer.invoke('build', values),
  pickIcon: () => ipcRenderer.invoke('pickIcon'),
  loadConfig: () => ipcRenderer.invoke('loadConfig'),
  saveConfig: (values) => ipcRenderer.invoke('saveConfig', values),
  onBuildStatus: (callback) => ipcRenderer.on('build-status', (_event, text) => callback(text)),
});
