// SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
// npm ci --prefix tests/mcp
// KDENLIVE_BINARY=/path/to/install/bin/kdenlive node tests/mcp/acceptance.mjs
// Set LD_LIBRARY_PATH for any privately installed Qt HTTP modules.
// Requires ffmpeg, ffprobe and melt. Runs an offscreen editor on a disposable project with the legacy D-Bus editing bridge disabled.
import assert from 'node:assert/strict';
import { spawn, execFile } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { createWriteStream } from 'node:fs';
import { readFile, writeFile, mkdir, mkdtemp, copyFile, symlink, stat } from 'node:fs/promises';
import { createServer } from 'node:net';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
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
const extraMedia = join(temporary, 'allowed media');
await mkdir(extraMedia);
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
await writeFile(config, `[startup]\nlastSeenVersionMajor=26\nlastSeenVersionMinor=8\n[env]\nmeltpath=/usr/bin/melt-7\nmltpath=/usr/share/mlt-7/profiles\nffmpegpath=/usr/bin/ffmpeg\nffprobepath=/usr/bin/ffprobe\n[unmanaged]\ndefault_profile=atsc_720p_30\n[MCP API]\nmcpEnabled=true\nmcpRequireToken=true\nmcpPort=${port}\nmcpMediaRoot=${pathToFileURL(extraMedia).href}\n`);
// Every registered tool with its expected annotations: [readOnlyHint, destructiveHint, idempotentHint].
const expectedTools = {
  desktop_capabilities: [true, false, true], desktop_state: [true, false, true], desktop_frame_capture: [true, false, true],
  desktop_effect_list: [true, false, true], desktop_title_read: [true, false, true], desktop_render_status: [true, false, true],
  desktop_apply: [false, true, false], desktop_media_import: [false, false, true], desktop_media_remove: [false, true, true],
  desktop_clip_insert: [false, false, false], desktop_clip_remove: [false, true, true], desktop_media_replace: [false, true, true],
  desktop_clip_move: [false, true, true], desktop_clip_trim: [false, true, true], desktop_audio_envelope: [false, false, true],
  desktop_track_rename: [false, true, true], desktop_project_save: [false, true, true], desktop_project_save_as: [false, false, true],
  desktop_project_profile: [false, true, true], desktop_clip_reframe: [false, true, true], desktop_effect_add: [false, false, false],
  desktop_effect_set: [false, true, true], desktop_effect_remove: [false, true, false], desktop_title_edit: [false, true, true],
  desktop_render: [false, false, false], desktop_undo: [false, true, false], desktop_redo: [false, true, false], desktop_batch: [false, true, false],
};
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
async function rejected(name, args = {}) {
  await refresh();
  const result = await tool(name, { ...fields(), ...args });
  assert.equal(result.ok, false, `${name} should fail: ${JSON.stringify(result)}`);
  return result.error.code;
}
async function capture(position, width) {
  const start = performance.now();
  const result = await client.callTool({ name: 'desktop_frame_capture', arguments: { position, width } });
  timings.push({ tool: 'desktop_frame_capture', milliseconds: Math.round(performance.now() - start) });
  assert.equal(result.structuredContent.ok, true, JSON.stringify(result.structuredContent));
  assert.equal(result.structuredContent.data.image, undefined, 'Image data must move to an MCP image block');
  const images = result.content.filter(item => item.type === 'image');
  assert.equal(images.length, 1); assert.equal(images[0].mimeType, 'image/png');
  const png = Buffer.from(images[0].data, 'base64');
  assert.equal(png.subarray(0, 8).toString('hex'), '89504e470d0a1a0a', 'Not a PNG');
  assert(png.length > 100 && png.length < 4_000_000, `Unexpected PNG size ${png.length}`);
  const { data } = result.structuredContent;
  assert.deepEqual([png.readUInt32BE(16), png.readUInt32BE(20)], [data.width, data.height]);
  assert.equal(data.position, position);
  return data;
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
  const names = catalog.tools.map(item => item.name).sort();
  const expectedNames = Object.keys(expectedTools).sort();
  assert.deepEqual(names, expectedNames, `Tool catalog mismatch. Missing: ${expectedNames.filter(name => !names.includes(name)).join(', ') || 'none'}; unexpected: ${names.filter(name => !expectedNames.includes(name)).join(', ') || 'none'}`);
  for (const item of catalog.tools) {
    const [readOnlyHint, destructiveHint, idempotentHint] = expectedTools[item.name];
    assert.deepEqual(item.annotations, { readOnlyHint, destructiveHint, idempotentHint, openWorldHint: false }, `Annotations of ${item.name}`);
    assert.equal(item.inputSchema.additionalProperties, false, `${item.name} accepts unknown arguments`);
    if (!readOnlyHint && item.name !== 'desktop_apply')
      for (const key of ['sessionId', 'expectedRevision', 'requestId']) assert(item.inputSchema.required.includes(key), `${item.name} must require ${key}`);
  }
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
  const allowedPath = join(extraMedia, 'allowed.mkv');
  await copyFile(source, allowedPath);
  const allowed = await edit('desktop_media_import', { path: allowedPath });
  await ready(allowed.binId, allowedPath);
  await edit('desktop_media_remove', { binId: allowed.binId });
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
  // Tools for reworking a project's format: capabilities, capture, effects, titles, batch, save-as, profile, reframe, render.
  const capabilities = await ok('desktop_capabilities');
  for (const operation of ['save_as', 'set_profile', 'reframe', 'effect_add', 'effect_set', 'effect_remove', 'title_edit', 'render', 'batch'])
    assert(capabilities.operations.includes(operation), `capabilities lacks ${operation}`);
  const videoTrack = state.tracks.find(track => !track.audio && track.clips.some(item => item.position === 420));
  const clipId = videoTrack.clips.find(item => item.position === 420).id;
  const vclip = () => clip(clipId);

  await refresh();
  const revisionBeforeCapture = state.revision;
  const frame = await capture(60, 320);
  assert.deepEqual([frame.width, frame.height], [320, 180]);
  await refresh(); assert.equal(state.revision, revisionBeforeCapture, 'Frame capture must not edit the project');
  assert.equal((await tool('desktop_frame_capture', { position: state.profile.duration })).error.code, 'INVALID_ARGUMENTS');

  const search = await ok('desktop_effect_list', { query: 'bright' });
  assert(search.catalog.some(item => item.effectId === 'brightness'), 'Catalog search misses brightness');
  assert(search.catalog.every(item => `${item.effectId} ${item.name}`.toLowerCase().includes('bright')), 'Catalog search returned non-matching effects');
  assert.equal(search.effects, undefined);
  const initial = await ok('desktop_effect_list', { clipId });
  assert.equal(initial.keyframeOrigin, vclip().sourceIn); assert.equal(initial.catalog, undefined);
  const baseEffects = initial.effects.length;
  assert.equal(baseEffects, vclip().effectCount);
  assert.deepEqual((await ok('desktop_effect_list', { binId: asset.id })).effects, []);
  assert.equal(await rejected('desktop_effect_add', { clipId, effectId: 'no_such_effect' }), 'UNKNOWN_EFFECT');
  const added = await edit('desktop_effect_add', { clipId, effectId: 'brightness', params: { level: '0.5' } });
  assert.equal(added.effectIndex, baseEffects); assert.equal(vclip().effectCount, baseEffects + 1);
  const effect = async () => (await ok('desktop_effect_list', { clipId })).effects[added.effectIndex];
  const brightness = await effect();
  assert.equal(brightness.effectId, 'brightness'); assert.equal(brightness.builtIn, false); assert.equal(brightness.enabled, true);
  assert.match(brightness.params.level, /(^|=)0\.5$/);
  assert.equal(await rejected('desktop_effect_set', { clipId, index: added.effectIndex, params: { no_such_parameter: '1' } }), 'UNKNOWN_PARAMETER');
  assert.equal(await rejected('desktop_effect_set', { clipId, index: 99, params: { level: '1' } }), 'UNKNOWN_EFFECT');
  await edit('desktop_effect_set', { clipId, index: added.effectIndex, params: { level: '0.8' } });
  assert.match((await effect()).params.level, /(^|=)0\.8$/);
  await edit('desktop_undo'); assert.match((await effect()).params.level, /(^|=)0\.5$/);
  await edit('desktop_redo'); assert.match((await effect()).params.level, /(^|=)0\.8$/);
  await edit('desktop_effect_remove', { clipId, index: added.effectIndex }); assert.equal(vclip().effectCount, baseEffects);
  await edit('desktop_undo'); assert.equal(vclip().effectCount, baseEffects + 1); assert.match((await effect()).params.level, /(^|=)0\.8$/);
  await edit('desktop_effect_remove', { clipId, index: added.effectIndex }); assert.equal(vclip().effectCount, baseEffects);

  const titleId = state.bin.find(item => item.name === 'MCP title').id;
  const readTitle = async () => {
    for (let attempt = 0; attempt < 150; attempt++) {
      const result = await tool('desktop_title_read', { binId: titleId });
      if (result.ok) return result.data;
      await delay(100);
    }
    throw Error('Title clip never became readable');
  };
  let title = await readTitle();
  assert.deepEqual([title.width, title.height, title.projectWidth, title.projectHeight], [1280, 720, 1280, 720]);
  assert.deepEqual(title.items.map(item => item.type), ['QGraphicsTextItem', 'QGraphicsRectItem']);
  assert.deepEqual([title.items[0].text, title.items[0].x, title.items[0].y, title.items[0].fontPixelSize, title.items[0].boxWidth, title.items[0].alignment],
    ['MCP title', 400, 300, 60, 480, 'left']);
  assert.equal(title.items[1].text, undefined);
  assert.equal((await tool('desktop_title_read', { binId: asset.id })).error.code, 'NOT_A_TITLE');
  assert.equal(await rejected('desktop_title_edit', { binId: titleId, items: [{ index: 1, text: 'not a text item' }] }), 'INVALID_TITLE');
  await edit('desktop_title_edit', { binId: titleId, items: [{ index: 0, text: 'Edited by MCP', x: 100, y: 200, fontPixelSize: 90, alignment: 'center' }] });
  title = await readTitle();
  assert.deepEqual([title.items[0].text, title.items[0].x, title.items[0].y, title.items[0].fontPixelSize, title.items[0].boxWidth, title.items[0].alignment],
    ['Edited by MCP', 100, 200, 90, 720, 'center']);
  await edit('desktop_undo'); assert.equal((await readTitle()).items[0].text, 'MCP title');
  await edit('desktop_redo'); assert.equal((await readTitle()).items[0].text, 'Edited by MCP');

  await refresh();
  const applied = await ok('desktop_apply', { request: { ...fields(), command: { type: 'rename_track', trackId: videoTrack.id, name: 'Applied' } } });
  state = applied.state;
  assert.equal(state.tracks.find(track => track.id === videoTrack.id).name, 'Applied');
  await edit('desktop_undo');

  await refresh();
  const beforeBatch = state;
  const batchCommands = [
    { type: 'rename_track', trackId: videoTrack.id, name: 'Batch V' },
    { type: 'move', clipId, trackId: videoTrack.id, position: 480 },
    { type: 'effect_add', clipId, effectId: 'brightness' },
  ];
  const batch = await edit('desktop_batch', { commands: batchCommands });
  assert.equal(batch.results.length, 3);
  assert.equal(state.undo.index, beforeBatch.undo.index + 1); assert.equal(state.undo.undoText, 'MCP batch (3 edits)');
  assert.equal(state.tracks.find(track => track.id === videoTrack.id).name, 'Batch V');
  assert.equal(vclip().position, 480); assert.equal(vclip().effectCount, baseEffects + 1);
  await edit('desktop_undo');
  assert.deepEqual(state.tracks, beforeBatch.tracks, 'One undo must revert the whole batch');
  await edit('desktop_redo'); assert.equal(vclip().position, 480);
  await edit('desktop_undo'); assert.equal(vclip().position, 420);
  // A failing step rolls back the earlier ones and leaves no history entry behind.
  await refresh();
  const beforeFailure = state;
  const failedBatch = await tool('desktop_batch', { ...fields(), commands: [...batchCommands, { type: 'remove_clip', clipId: 999999 }] });
  assert.equal(failedBatch.ok, false); assert.equal(failedBatch.error.code, 'UNKNOWN_CLIP'); assert.equal(failedBatch.failedIndex, 3);
  await refresh();
  assert.deepEqual(state.tracks, beforeFailure.tracks, 'Failed batch changed the timeline');
  assert.deepEqual(state.bin, beforeFailure.bin, 'Failed batch changed the bin');
  assert.deepEqual([state.undo.index, state.undo.undoText], [beforeFailure.undo.index, beforeFailure.undo.undoText], 'Failed batch changed the history');
  // Opening the batch macro discards the redo history like any edit, but the rolled-back batch must not become redoable.
  assert.equal(state.undo.canRedo, false, `Failed batch left a redo entry: ${state.undo.redoText}`);
  // Rolling back moves the undo stack, so the revision advances and clients must read state again.
  assert(state.revision > beforeFailure.revision);
  assert.equal(await rejected('desktop_batch', { commands: [{ type: 'save' }] }), 'INVALID_COMMAND');
  const revisionAfterFailure = state.revision;
  await refresh(); assert.equal(state.revision, revisionAfterFailure, 'A batch rejected before running must not advance the revision');

  assert.equal(await rejected('desktop_project_save_as', { path: join(temporary, 'outside.kdenlive') }), 'PATH_NOT_ALLOWED');
  assert.equal(await rejected('desktop_project_save_as', { path: project }), 'INVALID_PATH');
  const vertical = join(root, 'vertical.kdenlive');
  const sessionBeforeCopy = state.sessionId;
  const copy = await edit('desktop_project_save_as', { path: vertical });
  assert.equal(copy.documentUrl, pathToFileURL(vertical).href); assert.equal(state.documentUrl, copy.documentUrl);
  assert.equal(state.modified, false); assert((await stat(vertical)).size > 0);
  assert.equal(state.sessionId, sessionBeforeCopy, 'save_as keeps the editing session');

  const fps = state.fps;
  assert.equal(await rejected('desktop_project_profile', { width: 1081, height: 1920 }), 'INVALID_PROFILE');
  const profiled = await edit('desktop_project_profile', { width: 1080, height: 1920 });
  assert.equal(profiled.changed, true);
  assert.deepEqual([state.profile.width, state.profile.height], [1080, 1920]); assert.deepEqual(state.fps, fps);
  for (let attempt = 0; attempt < 150 && !state.bin.every(item => item.ready); attempt++) { await delay(100); await refresh(); }
  assert(state.bin.every(item => item.ready), 'Bin did not reload after the profile change');
  assert.equal((await edit('desktop_project_profile', { width: 1080, height: 1920 })).changed, false);

  const pan = await edit('desktop_clip_reframe', { clipId, mode: 'fill', focusX: 0, endFocusX: 1 });
  // A 320x180 source filling 1080x1920 is 3413x1920; the pan moves from the left edge to the right edge.
  const [inPoint, outPoint] = [vclip().sourceIn, vclip().sourceOut - 1];
  assert.equal(pan.rect, `${inPoint}=0 0 3413 1920 1;${outPoint}=-2333 0 3413 1920 1`);
  assert.equal(vclip().effectCount, baseEffects + 1);
  const transform = async () => (await ok('desktop_effect_list', { clipId })).effects[pan.effectIndex];
  assert.equal((await transform()).effectId, 'qtblend');
  const fit = await edit('desktop_clip_reframe', { clipId, mode: 'fit' });
  assert.equal(fit.effectIndex, pan.effectIndex, 'Reframing again reuses the Transform effect');
  assert.equal(fit.rect, `${inPoint}=0 656 1080 608 1`);
  assert.equal(vclip().effectCount, baseEffects + 1);
  await edit('desktop_undo'); assert.equal((await transform()).params.rect, pan.rect);
  await edit('desktop_redo'); assert.equal((await transform()).params.rect, fit.rect);
  await edit('desktop_undo');

  await edit('desktop_title_edit', { binId: titleId, width: 1080, height: 1920 });
  title = await readTitle();
  assert.deepEqual([title.width, title.height, title.projectWidth, title.projectHeight], [1080, 1920, 1080, 1920]);
  const verticalFrame = await capture(60, 270);
  assert.deepEqual([verticalFrame.width, verticalFrame.height], [270, 480]);

  const rendered = join(root, 'vertical.mp4');
  assert.equal(await rejected('desktop_render', { path: join(temporary, 'outside.mp4'), preset: 'MP4-H264/AAC' }), 'PATH_NOT_ALLOWED');
  assert.equal(await rejected('desktop_render', { path: join(root, 'wrong.mkv'), preset: 'MP4-H264/AAC' }), 'INVALID_PATH');
  assert.equal(await rejected('desktop_render', { path: rendered, preset: 'No such preset' }), 'UNKNOWN_PRESET');
  const render = await edit('desktop_render', { path: rendered, preset: 'MP4-H264/AAC' });
  assert.deepEqual(render.outputs, [rendered]); assert.equal(render.preset, 'MP4-H264/AAC');
  assert.equal(await rejected('desktop_render', { path: join(root, 'second.mp4'), preset: 'MP4-H264/AAC' }), 'RENDER_BUSY');
  let job;
  for (let attempt = 0; attempt < 240; attempt++) {
    job = (await ok('desktop_render_status')).jobs.find(item => item.path === rendered);
    assert(job, 'Render job is not listed');
    if (!['starting', 'running'].includes(job.status)) break;
    await delay(500);
  }
  assert.equal(job.status, 'finished', JSON.stringify(job));
  assert.equal(job.progress, 100); assert(job.bytes > 0);
  assert.equal((await stat(rendered)).size, job.bytes);
  const { stdout: dimensions } = await promisify(execFile)('ffprobe', ['-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=width,height', '-of', 'csv=p=0', rendered]);
  assert.equal(dimensions.trim(), '1080,1920');
  await writeFile(join(root, 'report.json'), JSON.stringify({ passed: true, transport: 'native Streamable HTTP', legacyBridge: false, tools: catalog.tools.map(tool => tool.name), timings, finalState: state }, null, 2));
  console.log(`Native MCP acceptance passed: ${root}/report.json`);
} catch (error) {
  console.error(`Failed after: ${timings.slice(-5).map(item => item.tool).join(' -> ')}; logs in ${root}`);
  throw error;
} finally { await stop(); }
