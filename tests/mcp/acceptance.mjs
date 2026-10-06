// SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
// npm ci --prefix tests/mcp
// KDENLIVE_BINARY=/path/to/install/bin/kdenlive node tests/mcp/acceptance.mjs
// Set LD_LIBRARY_PATH for any privately installed Qt HTTP modules.
// Requires ffmpeg and a desktop session. Edits a disposable project with the legacy D-Bus editing bridge disabled.
import assert from 'node:assert/strict';
import { spawn, execFile } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { createWriteStream } from 'node:fs';
import { readFile, writeFile, mkdir, mkdtemp, copyFile, symlink, stat } from 'node:fs/promises';
import { createServer } from 'node:net';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { setTimeout as delay } from 'node:timers/promises';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';

const directory = dirname(fileURLToPath(import.meta.url));
const binary = process.env.KDENLIVE_BINARY;
assert(binary, 'Set KDENLIVE_BINARY to the built editor');
const root = join(directory, 'artifacts', String(Date.now()));
await mkdir(root, { recursive: true });
for (const folder of ['config', 'data', 'cache', 'media']) await mkdir(join(root, folder));
const temporary = await mkdtemp('/tmp/kdenlive-http-test-');
const source = join(root, 'media/source.mkv');
const replacement = join(root, 'media/replacement.mkv');
const project = join(root, 'test.kdenlive');
const config = join(root, 'config/kdenliverc');
await promisify(execFile)('ffmpeg', ['-v', 'error', '-f', 'lavfi', '-i', 'color=c=blue:s=320x180:r=30', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '20', '-c:v', 'libx264', '-preset', 'ultrafast', '-threads', '1', '-c:a', 'pcm_s16le', source]);
await copyFile(source, replacement);
await writeFile(project, (await readFile(join(directory, 'fixture.kdenlive'), 'utf8')).replaceAll('@MEDIA@', source.replaceAll('&', '&amp;')));
const probe = createServer();
await new Promise((done, reject) => { probe.once('error', reject); probe.listen(0, '127.0.0.1', done); });
const port = probe.address().port;
await new Promise(done => probe.close(done));
await writeFile(config, `[startup]\nlastSeenVersionMajor=26\nlastSeenVersionMinor=8\n[env]\nmeltpath=/usr/bin/melt-7\nmltpath=/usr/share/mlt-7/profiles\nffmpegpath=/usr/bin/ffmpeg\nffprobepath=/usr/bin/ffprobe\n[unmanaged]\ndefault_profile=atsc_720p_30\n[MCP API]\nmcpEnabled=true\nmcpPort=${port}\n`);
const endpoint = new URL(`http://127.0.0.1:${port}/mcp`);
let editor, log, client, transport, state;
const timings = [];
const env = {
  ...process.env, KDENLIVE_MCP_BRIDGE: '0',
  XDG_CONFIG_HOME: join(root, 'config'), XDG_DATA_HOME: join(root, 'data'), XDG_CACHE_HOME: join(root, 'cache'),
  XDG_DATA_DIRS: `${resolve(dirname(binary), '../share')}:/usr/local/share:/usr/share`, TMPDIR: temporary,
  QT_QPA_PLATFORM: 'offscreen', QT_QPA_PLATFORMTHEME: '', QT_STYLE_OVERRIDE: 'Fusion', QT_QUICK_BACKEND: 'software',
  QT_LOGGING_RULES: '*.debug=false', MLT_NO_VDPAU: '1',
};
async function tool(name, args = {}) {
  const start = performance.now();
  const result = await client.callTool({ name, arguments: args });
  timings.push({ tool: name, milliseconds: Math.round(performance.now() - start) });
  assert(result.structuredContent, `Missing structured result for ${name}`);
  return result.structuredContent;
}
async function ok(name, args) {
  const result = await tool(name, args);
  assert.equal(result.ok, true, JSON.stringify(result));
  return result.data;
}
async function refresh() { state = await ok('desktop_state'); return state; }
const fields = () => ({ sessionId: state.sessionId, expectedRevision: state.revision, requestId: randomUUID() });
async function edit(name, args = {}) {
  await refresh();
  const result = await ok(name, { ...fields(), ...args });
  state = result.state;
  return result;
}
const clip = id => state.tracks.flatMap(track => track.clips).find(item => item.id === id);
async function ready(binId, url) {
  for (let attempt = 0; attempt < 150; attempt++) {
    await refresh();
    if (state.bin.some(item => item.id === binId && item.ready && item.url === url)) return;
    await delay(100);
  }
  throw Error(`Asset did not become ready: ${binId}`);
}
async function launch() {
  log = createWriteStream(join(root, `editor-${Date.now()}.log`));
  editor = spawn(binary, ['--config', config, '--no-welcome', project], { env, stdio: ['ignore', 'pipe', 'pipe'] });
  editor.stdout.pipe(log); editor.stderr.pipe(log);
  let last;
  for (let attempt = 0; attempt < 300; attempt++) {
    assert.equal(editor.exitCode, null, 'Editor exited early');
    try {
      const token = (await readFile(join(root, 'config/kdenlive/mcp-token'), 'utf8')).trim();
      client = new Client({ name: 'native-acceptance', version: '1.0' });
      transport = new StreamableHTTPClientTransport(endpoint, { requestInit: { headers: { Authorization: `Bearer ${token}` } } });
      await client.connect(transport);
      break;
    } catch (error) { last = error; await client?.close().catch(() => {}); client = undefined; }
    await delay(100);
  }
  assert(client, `MCP failed to start: ${last}`);
  for (let attempt = 0; attempt < 300; attempt++) {
    const result = await tool('desktop_state');
    if (result.ok && result.data.documentUrl.endsWith('/test.kdenlive') && result.data.bin.some(item => item.ready && item.url === source)) {
      state = result.data; return;
    }
    // After replacement, the original asset points to the replacement file.
    if (result.ok && result.data.bin.some(item => item.ready && item.url === replacement)) { state = result.data; return; }
    await delay(100);
  }
  throw Error('Project never became ready');
}
async function stop() {
  await transport?.terminateSession().catch(() => {});
  await client?.close().catch(() => {});
  client = undefined;
  if (editor && editor.exitCode === null && editor.signalCode === null) {
    const done = new Promise(resolveExit => editor.once('exit', resolveExit));
    editor.kill('SIGTERM');
    await Promise.race([done, delay(3000)]);
    if (editor.exitCode === null && editor.signalCode === null) editor.kill('SIGKILL');
  }
  log?.end();
}
try {
  await launch();
  const catalog = await client.listTools();
  assert.equal(catalog.tools.length, 15);
  const video = state.tracks.find(item => !item.audio && !item.locked);
  const audio = state.tracks.find(item => item.audio && item.clips.length);
  const asset = state.bin.find(item => item.ready && item.url === source);
  assert(video && audio && asset);
  const insertion = { ...fields(), binId: asset.id, trackId: video.id, position: 360, sourceIn: 30, sourceOut: 90, media: 'video' };
  const inserted = await ok('desktop_clip_insert', insertion);
  state = inserted.state;
  const id = inserted.clipId;
  assert.equal(clip(id).position, 360); assert.equal(clip(id).duration, 60);
  assert.deepEqual(await ok('desktop_clip_insert', insertion), inserted);
  await edit('desktop_clip_move', { clipId: id, trackId: video.id, position: 420 });
  assert.equal(clip(id).position, 420);
  assert.equal((await tool('desktop_clip_insert', { ...insertion, requestId: randomUUID() })).error.code, 'REVISION_CONFLICT');
  assert.equal((await tool('desktop_clip_insert', { ...insertion, position: 900 })).error.code, 'REQUEST_ID_REUSED');
  await edit('desktop_clip_trim', { clipId: id, duration: 45, edge: 'right' });
  assert.equal(clip(id).duration, 45);
  await edit('desktop_undo'); assert.equal(clip(id).duration, 60);
  await edit('desktop_redo'); assert.equal(clip(id).duration, 45);
  await edit('desktop_clip_remove', { clipId: id }); assert.equal(clip(id), undefined);
  await edit('desktop_undo'); assert.equal(clip(id).position, 420);
  await refresh();
  assert.equal((await tool('desktop_media_remove', { ...fields(), binId: asset.id })).error.code, 'ASSET_IN_USE');
  const outside = join(temporary, 'outside.mkv');
  await copyFile(source, outside);
  await symlink(outside, join(root, 'escape.mkv'));
  for (const path of [outside, join(root, 'escape.mkv')]) {
    await refresh();
    assert.equal((await tool('desktop_media_import', { ...fields(), path })).error.code, 'PATH_NOT_ALLOWED');
  }
  const imported = await edit('desktop_media_import', { path: replacement });
  await ready(imported.binId, replacement);
  assert.equal((await edit('desktop_media_import', { path: replacement })).binId, imported.binId);
  await edit('desktop_media_remove', { binId: imported.binId });
  assert(!state.bin.some(item => item.id === imported.binId)); assert((await stat(replacement)).size > 0);
  await edit('desktop_undo'); await ready(imported.binId, replacement);
  await edit('desktop_audio_envelope', { clipId: audio.clips[0].id, fadeIn: 30, fadeOut: 60, gainDb: -3 });
  assert.equal(clip(audio.clips[0].id).effectCount, 1);
  await edit('desktop_track_rename', { trackId: audio.id, name: 'MCP Music' });
  await refresh();
  const replaceRequest = { ...fields(), binId: asset.id, replacementBinId: imported.binId };
  const replaced = await ok('desktop_media_replace', replaceRequest);
  await ready(asset.id, replacement);
  assert.equal(clip(id).position, 420); assert.equal(clip(id).duration, 45);
  await edit('desktop_undo'); await ready(asset.id, source);
  await edit('desktop_redo'); await ready(asset.id, replacement);
  await edit('desktop_media_remove', { binId: imported.binId });
  // Replaying an already applied replacement must not re-check its removed source bin item.
  assert.deepEqual(await ok('desktop_media_replace', replaceRequest), replaced);
  await edit('desktop_project_save'); assert.equal(state.modified, false);
  assert.match(await readFile(project, 'utf8'), /name="kdenlive_id">volume<\/property>/);
  const previousSession = state.sessionId;
  await stop(); await launch();
  assert.notEqual(state.sessionId, previousSession);
  assert(state.tracks.flatMap(track => track.clips).some(item => item.position === 420 && item.duration === 45 && item.sourceIn === 30));
  assert.equal(state.tracks.find(track => track.name === 'MCP Music').clips[0].effectCount, 1);
  assert.equal((await tool('desktop_clip_insert', insertion)).error.code, 'SESSION_CHANGED');
  await writeFile(join(root, 'report.json'), JSON.stringify({ passed: true, transport: 'native Streamable HTTP', legacyBridge: false, tools: catalog.tools.map(tool => tool.name), timings, finalState: state }, null, 2));
  console.log(`Native MCP acceptance passed: ${root}/report.json`);
} finally { await stop(); }
