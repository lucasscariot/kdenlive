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
// A 20 s tone with silences at [3, 5) and [9, 12) seconds, for silence detection and cuts.
const speechFile = join(root, 'media/speech.wav');
await promisify(execFile)('ffmpeg', ['-v', 'error', '-f', 'lavfi', '-i', 'aevalsrc=if(gte(t\\,3)*lt(t\\,5)+gte(t\\,9)*lt(t\\,12)\\,0\\,0.5*sin(2*PI*440*t)):s=48000:d=20', '-c:a', 'pcm_s16le', speechFile]);
await writeFile(project, (await readFile(join(directory, 'fixture.kdenlive'), 'utf8')).replaceAll('@MEDIA@', source.replaceAll('&', '&amp;')));
// Kdenlive keeps a sequence's subtitles next to the project as <project file name>.ass.
await writeFile(`${project}.ass`, `[Script Info]\nScriptType: v4.00+\nPlayResX: 1280\nPlayResY: 720\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Sans,48,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:01.00,0:00:02.50,Default,,0,0,0,,Hello from MCP\nDialogue: 0,0:00:05.00,0:00:06.00,Default,,0,0,0,,Second line\n`);
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
  desktop_clip_insert: [false, true, false], desktop_clip_remove: [false, true, true], desktop_media_replace: [false, true, true],
  desktop_clip_move: [false, true, true], desktop_clip_trim: [false, true, true], desktop_audio_envelope: [false, false, true],
  desktop_track_rename: [false, true, true], desktop_project_save: [false, true, true], desktop_project_save_as: [false, false, true],
  desktop_project_profile: [false, true, true], desktop_clip_reframe: [false, true, true], desktop_effect_add: [false, false, false],
  desktop_effect_set: [false, true, true], desktop_effect_remove: [false, true, false], desktop_title_edit: [false, true, true],
  desktop_render: [false, false, false], desktop_undo: [false, true, false], desktop_redo: [false, true, false], desktop_batch: [false, true, false],
  desktop_marker_add: [false, true, true], desktop_marker_edit: [false, true, true], desktop_marker_remove: [false, true, true],
  desktop_marker_import: [false, true, true], desktop_marker_export: [true, false, true], desktop_history: [true, false, true],
  desktop_clip_split: [false, true, true], desktop_range_remove: [false, true, false], desktop_gap_remove: [false, true, false],
  desktop_space_insert: [false, true, false], desktop_clip_group: [false, false, true], desktop_clip_ungroup: [false, true, true],
  desktop_clip_speed: [false, true, true], desktop_clip_enable: [false, true, true],
  desktop_transcript: [true, false, true], desktop_silence_detect: [true, false, true], desktop_transcribe_status: [true, false, true],
  desktop_transcript_import: [false, true, false], desktop_range_cut: [false, true, false], desktop_transcribe: [false, true, false],
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
async function connect(name) {
  const token = (await readFile(join(root, 'config/kdenlive/mcp-token'), 'utf8')).trim();
  const connection = { client: new Client({ name, version: '1.0' }) };
  connection.transport = new StreamableHTTPClientTransport(endpoint, { requestInit: { headers: { Authorization: `Bearer ${token}` } } });
  await connection.client.connect(connection.transport);
  return connection;
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
  assert.equal(catalog.tools.length, 48);
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
  // State sections. The fixture carries a linked V1/A1 pair, a dissolve on Titles over V1, two guides, two subtitles,
  // a muted and locked empty audio track, and the title clip inside a Graphics bin folder.
  assert.deepEqual(state.sections, ['tracks', 'clips', 'compositions', 'markers', 'bin', 'sequences']);
  assert.deepEqual(state.counts, { tracks: 4, clips: 3, compositions: 1, markers: 2, subtitles: 2, bin: 4 });
  assert.equal(state.subtitles, undefined, 'Subtitles are opt-in');
  assert.equal(state.playhead, 0);
  assert.equal(state.lastChange, null, 'No MCP edit yet');
  const [v1, a1, titles] = state.tracks;
  assert.deepEqual(state.tracks.map(track => [track.name, track.type, track.locked, track.muted, track.hidden]),
    [['V1', 'video', false, false, false], ['A1', 'audio', false, false, false], ['Titles', 'video', false, false, false], ['Muted', 'audio', true, true, false]]);
  assert.deepEqual(state.tracks.filter(track => track.active).map(track => track.id), [state.activeTrackId]);
  const [linkedVideo, linkedAudio] = [v1.clips[0], a1.clips[0]];
  assert.deepEqual([linkedVideo.grouped, linkedVideo.linkedClipId, linkedAudio.linkedClipId, linkedVideo.enabled, linkedVideo.speed, linkedVideo.type],
    [true, linkedAudio.id, linkedVideo.id, true, 0.5, 'av']);
  assert.equal(typeof linkedVideo.groupId, 'number'); assert.equal(linkedAudio.groupId, linkedVideo.groupId);
  const background = titles.clips[0];
  assert.deepEqual([background.grouped, background.groupId, background.linkedClipId, background.type, background.mixes], [false, null, null, 'color', []]);
  assert.equal(background.effects, undefined, 'Effect summaries are opt-in');
  assert.deepEqual(titles.gaps, [{ position: 0, duration: 30 }]); assert.deepEqual(v1.gaps, []);
  assert.deepEqual(titles.compositions.map(({ id, ...rest }) => rest), [{ compositionId: 'dissolve', name: 'Dissolve', position: 30, duration: 60,
    aTrack: 1, aTrackId: v1.id, bTrack: 3, forcedATrack: false, grouped: false, groupId: null }]);
  assert.deepEqual(state.markers.map(({ color, categoryName, ...rest }) => rest),
    [{ position: 90, duration: 0, comment: 'Title in', category: 1 }, { position: 200, duration: 30, comment: 'Outro', category: 2 }]);
  assert(state.markers.every(marker => /^#[0-9a-f]{6}$/.test(marker.color) && marker.categoryName), JSON.stringify(state.markers));
  const folder = name => state.folders.find(item => item.name === name);
  assert.equal(folder('Graphics').parentId, null);
  const titleAsset = state.bin.find(item => item.name === 'MCP title');
  assert.deepEqual([titleAsset.type, titleAsset.parentId], ['title', folder('Graphics').id]);
  assert.deepEqual(['color', 'av'].map(type => state.bin.find(item => item.type === type)?.parentId), [null, null]);
  assert.equal(state.bin.find(item => item.type === 'color').url, '');
  const sequenceAsset = state.bin.find(item => item.type === 'sequence');
  assert.equal(sequenceAsset.parentId, folder('Sequences').id);
  assert.deepEqual(state.sequences, [{ id: state.sequenceId, binId: sequenceAsset.id, name: sequenceAsset.name, active: true, open: true }]);
  const subtitled = await ok('desktop_state', { include: ['subtitles'] });
  assert.deepEqual(subtitled.sections, ['subtitles']);
  for (const key of ['tracks', 'markers', 'bin', 'folders', 'sequences']) assert.equal(subtitled[key], undefined, `include omits ${key}`);
  assert.deepEqual([subtitled.sessionId, subtitled.counts], [state.sessionId, state.counts]);
  assert.deepEqual(subtitled.subtitles.map(({ id, ...rest }) => rest),
    [{ layer: 0, start: 30, end: 75, text: 'Hello from MCP' }, { layer: 0, start: 150, end: 180, text: 'Second line' }]);
  const scoped = await ok('desktop_state', { include: ['compositions', 'markers', 'subtitles'], trackId: titles.id, range: { start: 80, end: 160 } });
  assert.deepEqual(scoped.sections, ['tracks', 'compositions', 'markers', 'subtitles']);
  assert.deepEqual(scoped.filter, { trackId: titles.id, range: { start: 80, end: 160 } });
  assert.deepEqual(scoped.tracks.map(track => [track.id, track.clips, track.compositions.length]), [[titles.id, undefined, 1]]);
  assert.deepEqual(scoped.markers.map(marker => marker.position), [90]);
  assert.deepEqual(scoped.subtitles.map(subtitle => subtitle.start), [150]);
  assert.deepEqual((await ok('desktop_state', { trackId: titles.id, range: { start: 0, end: 20 } })).tracks[0], { ...titles, clips: [], compositions: [] });
  const media = await ok('desktop_state', { include: ['media'] });
  assert.deepEqual(media.sections, ['bin', 'media']);
  const sourceMedia = media.bin.find(item => item.url === source);
  assert.deepEqual([sourceMedia.width, sourceMedia.height, sourceMedia.fps, sourceMedia.hasVideo, sourceMedia.hasAudio], [320, 180, 30, true, true]);
  for (const args of [{ include: ['nope'] }, { include: 'tracks' }, { range: { start: 10, end: 10 } }, { range: { start: 0 } }, { unknown: 1 }])
    assert.equal((await tool('desktop_state', args)).error.code, 'INVALID_ARGUMENTS', JSON.stringify(args));
  assert.equal((await tool('desktop_state', { trackId: 999999 })).error.code, 'UNKNOWN_TRACK');

  // Markers and guides: categories, add/edit/remove with undo, bulk removal as one step, export/import round trips, bin-clip markers.
  const markerCaps = await ok('desktop_capabilities');
  for (const operation of ['marker_add', 'marker_edit', 'marker_remove', 'marker_import']) assert(markerCaps.operations.includes(operation), operation);
  assert.deepEqual(markerCaps.markerCategories.map(category => category.index), [0, 1, 2, 3, 4, 5, 6, 7, 8]);
  assert(markerCaps.markerCategories.every(category => category.name && /^#[0-9a-f]{6}$/.test(category.color)), JSON.stringify(markerCaps.markerCategories));
  assert.deepEqual([markerCaps.defaultMarkerCategory, markerCaps.markerFormats], [0, ['json', 'csv', 'kdenlive']]);
  const category = index => markerCaps.markerCategories.find(item => item.index === index);
  const guide = position => state.markers.find(marker => marker.position === position);
  const fixtureGuides = state.markers;
  // Native history entries carry an hh:mm prefix.
  const undoLabel = () => state.undo.undoText.replace(/^\d\d:\d\d /, '');
  const sourceBin = () => state.bin.find(item => item.url === source);
  assert.deepEqual(sourceBin().markers, [{ position: 15, duration: 0, comment: 'Clap', category: 3, categoryName: category(3).name, color: category(3).color }]);
  assert(state.bin.filter(item => item.url !== source).every(item => item.markers === undefined), 'Bin items without markers, and sequences, omit them');
  let marker = await edit('desktop_marker_add', { position: 30, comment: 'Cut here', category: 4 });
  assert.deepEqual([marker.target, marker.replaced, marker.binId], ['guides', false, undefined]);
  assert.deepEqual(marker.marker, { position: 30, duration: 0, comment: 'Cut here', category: 4, categoryName: category(4).name, color: category(4).color });
  assert.deepEqual(guide(30), marker.marker); assert.equal(undoLabel(), 'Add guide'); assert.equal(state.counts.markers, 3);
  assert.deepEqual(state.lastChange, { undoIndex: state.undo.index, command: 'marker_add', text: 'Add guide', requestId: marker.requestId, applied: true });
  marker = await edit('desktop_marker_add', { position: 120, duration: 45, comment: 'Interview', category: category(5).name.toUpperCase() });
  assert.deepEqual([marker.marker.duration, marker.marker.category], [45, 5], 'Range guide with a case-insensitive category name');
  marker = await edit('desktop_marker_add', { position: 60 });
  // Kdenlive reads an empty comment back as its default label.
  assert.deepEqual([marker.marker.comment, marker.marker.category], ['Marker', markerCaps.defaultMarkerCategory]);
  await edit('desktop_undo'); assert.equal(guide(60), undefined);
  await edit('desktop_redo'); assert.equal(guide(60).category, markerCaps.defaultMarkerCategory);
  marker = await edit('desktop_marker_add', { position: 30, comment: 'Cut here, "v2"', category: 4 });
  assert.equal(marker.replaced, true); assert.equal(undoLabel(), 'Replace guide'); assert.equal(state.counts.markers, 5);
  await edit('desktop_undo'); assert.equal(guide(30).comment, 'Cut here');
  await edit('desktop_redo'); assert.equal(guide(30).comment, 'Cut here, "v2"');
  marker = await edit('desktop_marker_edit', { position: 120, newPosition: 130, duration: 20, comment: 'Interview B', category: 6 });
  assert.deepEqual([marker.previous.position, marker.previous.duration, marker.previous.comment, marker.previous.category], [120, 45, 'Interview', 5]);
  assert.deepEqual([marker.marker.position, marker.marker.duration, marker.marker.comment, marker.marker.category, marker.changed], [130, 20, 'Interview B', 6, true]);
  assert.equal(undoLabel(), 'Move guide'); assert.equal(guide(120), undefined);
  await edit('desktop_undo'); assert.deepEqual([guide(120).duration, guide(120).category, guide(130)], [45, 5, undefined]);
  await edit('desktop_redo'); assert.deepEqual([guide(130).duration, guide(120)], [20, undefined]);
  await edit('desktop_marker_edit', { position: 130, duration: 0 });
  assert.deepEqual([guide(130).duration, guide(130).comment, undoLabel()], [0, 'Interview B', 'Edit guide']);
  await edit('desktop_undo'); assert.equal(guide(130).duration, 20);
  await edit('desktop_redo'); assert.equal(guide(130).duration, 0);
  assert.equal(await rejected('desktop_marker_edit', { position: 131, comment: 'nothing here' }), 'UNKNOWN_MARKER');
  assert.equal(await rejected('desktop_marker_edit', { position: 130, newPosition: 90 }), 'MARKER_EXISTS');
  assert.equal(await rejected('desktop_marker_edit', { position: 130 }), 'INVALID_COMMAND');
  assert.equal(await rejected('desktop_marker_add', { position: 10, category: 99 }), 'UNKNOWN_CATEGORY');
  assert.equal(await rejected('desktop_marker_add', { position: 10, category: 'No such category' }), 'UNKNOWN_CATEGORY');
  for (const args of [{ position: -1 }, { position: state.profile.duration }, { position: state.profile.duration - 10, duration: 20 }, { position: 1.5 }])
    assert.equal(await rejected('desktop_marker_add', args), 'INVALID_ARGUMENTS', JSON.stringify(args));
  assert.equal(await rejected('desktop_marker_edit', { position: 130, newPosition: state.profile.duration }), 'INVALID_ARGUMENTS');
  assert.equal(await rejected('desktop_marker_add', { position: 10, binId: sequenceAsset.id }), 'SEQUENCE_PROTECTED');
  assert.equal(await rejected('desktop_marker_remove', { position: 31 }), 'UNKNOWN_MARKER');
  assert.equal(await rejected('desktop_marker_remove', { category: 7 }), 'UNKNOWN_MARKER');
  for (const args of [{}, { position: 30, all: true }, { all: true, category: 4 }, { all: false }])
    assert.equal(await rejected('desktop_marker_remove', args), 'INVALID_COMMAND', JSON.stringify(args));
  assert.equal(await rejected('desktop_marker_remove', { range: { start: 50, end: 50 } }), 'INVALID_ARGUMENTS');
  const guideSet = state.markers;
  assert.deepEqual(guideSet.map(item => item.position), [30, 60, 90, 130, 200]);
  marker = await edit('desktop_marker_remove', { position: 60 });
  assert.deepEqual([marker.count, marker.removed[0].position, undoLabel(), guide(60)], [1, 60, 'Remove guide', undefined]);
  await edit('desktop_undo'); assert.deepEqual(state.markers, guideSet);
  marker = await edit('desktop_marker_remove', { range: { start: 60, end: 200 } });
  assert.deepEqual([marker.removed.map(item => item.position), undoLabel()], [[60, 90, 130], 'Remove 3 guides']);
  await edit('desktop_undo'); assert.deepEqual(state.markers, guideSet);
  marker = await edit('desktop_marker_remove', { category: category(1).name, range: { start: 0, end: 100 } });
  assert.deepEqual(marker.removed.map(item => item.position), [90]);
  await edit('desktop_undo');
  // Export in every format, remove everything in one Undo step, then import each export back.
  const exports = {};
  for (const format of markerCaps.markerFormats) {
    exports[format] = await ok('desktop_marker_export', { format });
    assert.deepEqual([exports[format].format, exports[format].target, exports[format].count], [format, 'guides', 5]);
  }
  assert.deepEqual(JSON.parse(exports.json.text), guideSet);
  const csv = exports.csv.text.trimEnd().split('\n');
  assert.equal(csv[0], 'position,timecode,duration,category,categoryName,color,comment');
  assert.equal(csv[1], `30,00:00:01:00,0,4,${category(4).name},${category(4).color},"Cut here, ""v2"""`);
  assert.equal(csv[5], `200,00:00:06:20,30,2,${category(2).name},${category(2).color},Outro`);
  assert.deepEqual(JSON.parse(exports.kdenlive.text).map(item => [item.pos, item.type, item.duration]), [[30, 4, 0], [60, 0, 0], [90, 1, 0], [130, 6, 0], [200, 2, 30]]);
  assert.equal((await tool('desktop_marker_export', { format: 'xml' })).error.code, 'INVALID_ARGUMENTS');
  const beforeRemoveAll = state.undo.index;
  marker = await edit('desktop_marker_remove', { all: true });
  assert.deepEqual([marker.count, state.markers, state.counts.markers, undoLabel()], [5, [], 0, 'Remove 5 guides']);
  assert.equal(state.undo.index, beforeRemoveAll + 1, 'Removing many guides adds one history entry');
  for (const format of markerCaps.markerFormats) {
    const imported = await edit('desktop_marker_import', { format, text: exports[format].text });
    assert.deepEqual([imported.imported, imported.replaced, undoLabel()], [5, 0, 'Import 5 guides'], format);
    assert.deepEqual(state.markers, guideSet, `${format} export/import round trip`);
    await edit('desktop_undo'); assert.deepEqual(state.markers, [], `${format} import undoes in one step`);
  }
  await edit('desktop_undo'); assert.deepEqual(state.markers, guideSet, 'One undo restores every removed guide');
  marker = await edit('desktop_marker_import', { format: 'csv', text: `timecode,comment,categoryName\n00:00:03:00,"Title in, again",${category(8).name}\n` });
  assert.deepEqual([marker.imported, marker.replaced, guide(90).comment, guide(90).category], [1, 1, 'Title in, again', 8]);
  await edit('desktop_undo'); assert.deepEqual(state.markers, guideSet);
  for (const [format, text] of [['json', 'not json'], ['json', '[{"position": 10}, {"position": 99999}]'], ['csv', 'comment\nx\n'],
    ['csv', 'position,comment\n10,"unterminated\n'], ['kdenlive', '[{"position": 10}]'], ['json', '[]']])
    assert.equal(await rejected('desktop_marker_import', { format, text }), 'INVALID_ARGUMENTS', `${format}: ${text}`);
  assert.equal(await rejected('desktop_marker_import', { format: 'json', text: '[{"position": 10, "category": "No such category"}]' }), 'UNKNOWN_CATEGORY');
  assert.deepEqual(state.markers, guideSet, 'Rejected imports change nothing');
  const markerBatch = await edit('desktop_batch', { commands: [{ type: 'marker_add', position: 10, comment: 'Batch' }, { type: 'marker_remove', position: 30 },
    { type: 'marker_edit', position: 60, comment: 'Batch edit' }] });
  assert.deepEqual([markerBatch.results.length, undoLabel(), guide(10).comment, guide(30), guide(60).comment], [3, 'MCP batch (3 edits)', 'Batch', undefined, 'Batch edit']);
  await edit('desktop_undo'); assert.deepEqual(state.markers, guideSet);
  // Bin-clip markers use frames relative to the clip and appear on the bin item.
  const binId = sourceBin().id;
  marker = await edit('desktop_marker_add', { binId, position: 45, duration: 15, comment: 'Laugh', category: category(2).name });
  assert.deepEqual([marker.target, marker.binId, undoLabel()], ['clip', binId, 'Add clip marker']);
  assert.deepEqual(sourceBin().markers.map(item => [item.position, item.duration, item.comment, item.category]), [[15, 0, 'Clap', 3], [45, 15, 'Laugh', 2]]);
  assert.deepEqual(state.markers, guideSet, 'Clip markers are not guides');
  await edit('desktop_marker_edit', { binId, position: 45, comment: 'Big laugh' });
  assert.equal(sourceBin().markers[1].comment, 'Big laugh');
  await edit('desktop_undo'); assert.equal(sourceBin().markers[1].comment, 'Laugh');
  const clipExport = await ok('desktop_marker_export', { format: 'csv', binId });
  assert.deepEqual([clipExport.target, clipExport.binId, clipExport.count], ['clip', binId, 2]);
  assert.equal(await rejected('desktop_marker_add', { binId, position: sourceBin().duration }), 'INVALID_ARGUMENTS');
  assert.equal(await rejected('desktop_marker_remove', { binId, position: 16 }), 'UNKNOWN_MARKER');
  marker = await edit('desktop_marker_remove', { binId, all: true });
  assert.deepEqual([marker.count, sourceBin().markers, undoLabel()], [2, undefined, 'Remove 2 clip markers']);
  await edit('desktop_undo'); assert.equal(sourceBin().markers.length, 2);
  await edit('desktop_marker_import', { binId, format: 'csv', text: clipExport.text });
  assert.equal(sourceBin().markers.length, 2, 'Clip marker CSV re-import replaces in place');
  await edit('desktop_marker_remove', { binId, position: 45 });
  // Restore the fixture guides from the json state shape, extra keys included.
  await edit('desktop_marker_remove', { all: true });
  await edit('desktop_marker_import', { format: 'json', text: JSON.stringify(fixtureGuides) });
  assert.deepEqual(state.markers, fixtureGuides); assert.equal(state.counts.markers, 2);
  // Timeline primitives: split, lift and extract, gaps and space, insert modes with linked A/V, groups, speed and enable. Every edit is undone
  // again, so the fixture layout comes back at the end of the section.
  await refresh();
  const timelineCaps = await ok('desktop_capabilities');
  for (const operation of ['split', 'remove_range', 'remove_gap', 'insert_space', 'group', 'ungroup', 'speed', 'enable'])
    assert(timelineCaps.operations.includes(operation), operation);
  assert.deepEqual([timelineCaps.editModes, timelineCaps.removeModes], [['normal', 'overwrite', 'insert'], ['lift', 'extract']]);
  const lane = name => state.tracks.find(item => item.name === name);
  const spans = name => lane(name).clips.map(item => [item.position, item.duration]);
  const layout = () => state.tracks.map(item => [item.name, item.gaps, item.compositions.map(c => [c.id, c.position, c.duration]),
    item.clips.map(c => [c.id, c.position, c.duration, c.sourceIn, c.sourceOut, c.speed, c.enabled, c.linkedClipId, c.grouped])]);
  const timelineStart = layout();
  const timelineIndex = state.undo.index;
  const [V1, A1, Titles, Muted] = ['V1', 'A1', 'Titles', 'Muted'].map(lane);
  const [pairV, pairA, colorClip] = [V1.clips[0].id, A1.clips[0].id, Titles.clips[0].id];
  const lastSummary = async () => (await ok('desktop_history', { limit: 1 })).entries[0];
  // Split one clip of the linked pair: both halves are cut and form two linked pairs; Undo restores one clip.
  let cut = await edit('desktop_clip_split', { clipId: pairV, position: 100 });
  assert.equal(undoLabel(), 'Cut clip');
  assert.deepEqual([spans('V1'), spans('A1')], [[[0, 100], [100, 200]], [[0, 100], [100, 200]]]);
  const [leftV, rightV] = lane('V1').clips;
  const [leftA, rightA] = lane('A1').clips;
  assert.deepEqual([cut.leftClipId, cut.rightClipId, leftV.id, leftA.id], [pairV, rightV.id, pairV, pairA]);
  assert.deepEqual(cut.pieces.map(piece => [piece.trackId, piece.leftClipId, piece.rightClipId]).sort(),
    [[V1.id, pairV, rightV.id], [A1.id, pairA, rightA.id]].sort());
  assert.deepEqual(cut.newClipIds, [rightV.id, rightA.id].sort((a, b) => a - b));
  assert.deepEqual([leftV.linkedClipId, rightV.linkedClipId, rightV.speed, rightV.sourceIn], [pairA, rightA.id, 0.5, leftV.sourceOut]);
  assert.notEqual(leftV.groupId, rightV.groupId); assert.equal(rightV.groupId, rightA.groupId);
  assert.deepEqual((await lastSummary()).summary.pieces.sort(), [[pairV, rightV.id], [pairA, rightA.id]].sort());
  await edit('desktop_undo'); assert.deepEqual(layout(), timelineStart, 'Undo restores the unsplit pair');
  for (const [args, code] of [[{ clipId: pairV, position: 0 }, 'NOT_SPLITTABLE'], [{ clipId: pairV, position: 300 }, 'NOT_SPLITTABLE'],
    [{ trackId: Titles.id, position: 10 }, 'NOT_SPLITTABLE'], [{ trackId: Muted.id, position: 10 }, 'TRACK_LOCKED'], [{ position: 10 }, 'INVALID_COMMAND'],
    [{ clipId: pairV, allTracks: true, position: 10 }, 'INVALID_COMMAND'], [{ clipId: 999999, position: 10 }, 'UNKNOWN_CLIP']])
    assert.equal(await rejected('desktop_clip_split', args), code, JSON.stringify(args));
  cut = await edit('desktop_clip_split', { position: 150, allTracks: true });
  assert.deepEqual([cut.pieces.length, cut.newClipIds.length, cut.leftClipId, undoLabel()], [3, 3, undefined, 'Cut all clips']);
  assert.deepEqual([spans('V1'), spans('Titles')], [[[0, 150], [150, 150]], [[30, 120], [150, 120]]]);
  await edit('desktop_undo'); assert.deepEqual(layout(), timelineStart);
  // Lift leaves a gap; extract closes it on the clip's track; the whole group goes unless group is single.
  await edit('desktop_clip_split', { clipId: pairV, position: 100 });
  const rightPieces = [lane('V1').clips[1].id, lane('A1').clips[1].id];
  let removed = await edit('desktop_clip_remove', { clipId: pairV });
  assert.deepEqual([removed.mode, removed.removedClipIds, undoLabel()], ['lift', [pairV, pairA].sort((a, b) => a - b), 'Remove group']);
  assert.deepEqual([lane('V1').gaps, lane('A1').gaps, spans('V1')], [[{ position: 0, duration: 100 }], [{ position: 0, duration: 100 }], [[100, 200]]]);
  assert.deepEqual((await lastSummary()).summary.removedClipIds, removed.removedClipIds);
  await edit('desktop_undo');
  removed = await edit('desktop_clip_remove', { clipId: pairV, mode: 'extract' });
  assert.deepEqual([undoLabel(), spans('V1'), spans('A1'), spans('Titles')], ['Extract clip', [[0, 200]], [[0, 200]], [[30, 240]]]);
  assert.deepEqual(removed.ranges.map(range => [range.start, range.end]), [[0, 100], [0, 100]]);
  assert.deepEqual([lane('V1').clips[0].id, lane('V1').clips[0].linkedClipId], [rightPieces[0], rightPieces[1]], 'Later clips move and stay linked');
  await edit('desktop_undo');
  removed = await edit('desktop_clip_remove', { clipId: pairV, mode: 'extract', group: 'single' });
  assert.deepEqual([removed.removedClipIds, spans('V1'), spans('A1')], [[pairV], [[0, 200]], [[0, 100], [100, 200]]]);
  assert.equal(clip(pairA).linkedClipId, null, 'single takes the clip out of its group');
  await edit('desktop_undo');
  removed = await edit('desktop_clip_remove', { clipId: pairV, mode: 'extract', allTracks: true });
  assert.deepEqual([undoLabel(), spans('V1'), spans('Titles'), lane('Titles').compositions, removed.guidesMoved], ['Extract zone', [[0, 200]], [[0, 170]], [], false]);
  assert.deepEqual(state.markers.map(item => item.position), [90, 200], 'Guides stay while Kdenlive locks guides');
  await edit('desktop_undo'); await edit('desktop_undo');
  assert.deepEqual(layout(), timelineStart);
  for (const [args, code] of [[{ clipId: pairV, mode: 'ripple' }, 'INVALID_COMMAND'], [{ clipId: pairV, allTracks: true }, 'INVALID_COMMAND'],
    [{ clipId: pairV, group: 'some' }, 'INVALID_COMMAND']])
    assert.equal(await rejected('desktop_clip_remove', args), code, JSON.stringify(args));
  // Range removal: a split and an extract in one batch are one Undo step; ranges on linked tracks keep the pairs linked.
  const timelineBatch = await edit('desktop_batch', { commands: [{ type: 'split', trackId: V1.id, position: 100 },
    { type: 'remove_range', start: 100, end: 150, trackIds: [V1.id, A1.id], mode: 'extract' }] });
  assert.deepEqual([undoLabel(), state.undo.index, spans('V1'), spans('A1'), spans('Titles')],
    ['MCP batch (2 edits)', timelineIndex + 1, [[0, 100], [100, 150]], [[0, 100], [100, 150]], [[30, 240]]]);
  assert.deepEqual(lane('V1').clips.map(item => clip(item.linkedClipId).position), [0, 100]);
  const batchEntry = await lastSummary();
  assert.deepEqual(batchEntry.children.map(child => child.type), ['split', 'remove_range']);
  assert.equal(batchEntry.children[0].summary.pieces.length, 2);
  assert.deepEqual(batchEntry.children[1].summary.removedClipIds, timelineBatch.results[1].removedClipIds);
  assert.deepEqual([timelineBatch.results[1].removedClipIds.length, timelineBatch.results[1].newClipIds.length], [2, 2]);
  await edit('desktop_undo'); assert.deepEqual(layout(), timelineStart, 'One Undo reverts the split and the extract');
  let range = await edit('desktop_range_remove', { start: 50, end: 80, trackIds: [V1.id, A1.id] });
  assert.deepEqual([range.mode, undoLabel(), lane('V1').gaps, lane('A1').gaps, spans('V1')], ['lift', 'Lift zone', [{ position: 50, duration: 30 }], [{ position: 50, duration: 30 }], [[0, 50], [80, 220]]]);
  // Close the gap on V1: its linked audio moves with it.
  const gap = await edit('desktop_gap_remove', { trackId: V1.id, position: 60 });
  assert.deepEqual([gap.removed, undoLabel(), spans('V1'), spans('A1')], [30, 'Remove space', [[0, 50], [50, 220]], [[0, 50], [50, 220]]]);
  await edit('desktop_undo'); await edit('desktop_undo');
  range = await edit('desktop_range_remove', { start: 0, end: 60, allTracks: true, mode: 'extract' });
  // As in the GUI, a composition overlapping a lifted range is removed with it.
  assert.deepEqual([undoLabel(), spans('V1'), spans('Titles'), lane('Titles').compositions], ['Extract zone', [[0, 240]], [[0, 210]], []]);
  await edit('desktop_undo'); assert.deepEqual(layout(), timelineStart);
  for (const [args, code] of [[{ start: 10, end: 10, allTracks: true }, 'INVALID_COMMAND'], [{ start: 0, end: 10 }, 'INVALID_COMMAND'],
    [{ start: 0, end: 10, trackIds: [Muted.id] }, 'TRACK_LOCKED'], [{ start: 0, end: 10, trackIds: [999999] }, 'UNKNOWN_TRACK']])
    assert.equal(await rejected('desktop_range_remove', args), code, JSON.stringify(args));
  // Gaps and space: the Titles gap before its clip, a composition moving with it, and refusals.
  await edit('desktop_gap_remove', { trackId: Titles.id, position: 10 });
  assert.deepEqual([spans('Titles'), lane('Titles').gaps, lane('Titles').compositions[0].position], [[[0, 240]], [], 0]);
  await edit('desktop_undo');
  for (const [args, code] of [[{ trackId: V1.id, position: 10 }, 'NO_GAP'], [{ trackId: Titles.id, position: 280 }, 'NO_GAP'],
    [{ allTracks: true, position: 10 }, 'NO_GAP'], [{ trackId: Muted.id, position: 10 }, 'TRACK_LOCKED'], [{ position: 10 }, 'INVALID_COMMAND']])
    assert.equal(await rejected('desktop_gap_remove', args), code, JSON.stringify(args));
  let space = await edit('desktop_space_insert', { trackId: Titles.id, position: 0, duration: 20 });
  assert.deepEqual([undoLabel(), spans('Titles'), spans('V1'), space.guidesMoved], ['Insert space', [[50, 240]], [[0, 300]], false]);
  await edit('desktop_undo');
  space = await edit('desktop_space_insert', { allTracks: true, position: 0, duration: 10 });
  assert.deepEqual([spans('V1'), spans('A1'), spans('Titles')], [[[10, 300]], [[10, 300]], [[40, 240]]]);
  await edit('desktop_undo'); assert.deepEqual(layout(), timelineStart);
  for (const [args, code] of [[{ trackId: Titles.id, position: 280, duration: 5 }, 'NOTHING_TO_MOVE'], [{ trackId: Muted.id, position: 0, duration: 5 }, 'TRACK_LOCKED'],
    [{ trackId: V1.id, position: 0, duration: 0 }, 'INVALID_COMMAND'], [{ trackId: V1.id, allTracks: true, position: 0, duration: 5 }, 'INVALID_COMMAND']])
    assert.equal(await rejected('desktop_space_insert', args), code, JSON.stringify(args));
  // Linked insertion places the audio on the nearest unlocked audio track and links both halves.
  const sourceAsset = state.bin.find(item => item.ready && item.url === source);
  const place = { binId: sourceAsset.id, trackId: V1.id, sourceIn: 0, sourceOut: 60, media: 'video' };
  let placed = await edit('desktop_clip_insert', { ...place, position: 400 });
  assert.deepEqual([placed.mode, placed.linked, undoLabel(), clip(placed.audioClipId).position, clip(placed.audioClipId).duration], ['normal', true, 'Insert Clip', 400, 60]);
  assert.equal(lane('A1').clips.some(item => item.id === placed.audioClipId), true);
  assert.deepEqual([clip(placed.clipId).linkedClipId, clip(placed.audioClipId).linkedClipId, clip(placed.clipId).groupId, clip(placed.audioClipId).groupId],
    [placed.audioClipId, placed.clipId, placed.groupId, placed.groupId]);
  const insertEntry = await lastSummary();
  assert.deepEqual([insertEntry.summary.clipId, insertEntry.summary.audioClipId, insertEntry.summary.groupId, insertEntry.summary.mode], [placed.clipId, placed.audioClipId, placed.groupId, 'normal']);
  await edit('desktop_undo'); assert.deepEqual(layout(), timelineStart);
  assert.equal(await rejected('desktop_clip_insert', { ...place, position: 100 }), 'OVERLAP');
  placed = await edit('desktop_clip_insert', { ...place, position: 100, mode: 'insert' });
  assert.deepEqual([undoLabel(), spans('V1'), spans('A1'), spans('Titles')], ['Insert clip (ripple)', [[0, 100], [100, 60], [160, 200]], [[0, 100], [100, 60], [160, 200]], [[30, 240]]]);
  assert.deepEqual([lane('V1').clips[1].id, lane('A1').clips[1].id], [placed.clipId, placed.audioClipId], 'Later clips shift right');
  assert.equal(clip(lane('V1').clips[2].id).linkedClipId, lane('A1').clips[2].id);
  await edit('desktop_undo');
  await edit('desktop_clip_insert', { ...place, position: 100, mode: 'insert', allTracks: true });
  assert.deepEqual(spans('Titles'), [[30, 70], [160, 170]], 'allTracks ripples every unlocked track');
  await edit('desktop_undo');
  placed = await edit('desktop_clip_insert', { ...place, position: 100, mode: 'overwrite' });
  assert.deepEqual([undoLabel(), spans('V1'), spans('A1')], ['Overwrite clip', [[0, 100], [100, 60], [160, 140]], [[0, 100], [100, 60], [160, 140]]]);
  await edit('desktop_undo'); assert.deepEqual(layout(), timelineStart);
  placed = await edit('desktop_clip_insert', { ...place, trackId: A1.id, position: 400, media: 'audio' });
  assert.deepEqual([placed.linked, clip(placed.clipId).position, lane('A1').clips.at(-1).id], [false, 400, placed.clipId], 'Audio-only insertion');
  await edit('desktop_undo');
  placed = await edit('desktop_clip_insert', { ...place, position: 400, linked: false });
  assert.deepEqual([placed.linked, placed.audioClipId, clip(placed.clipId).linkedClipId], [false, undefined, null]);
  await edit('desktop_undo');
  const colorAsset = state.bin.find(item => item.type === 'color');
  for (const [args, code] of [[{ ...place, position: 400, media: 'audio' }, 'INCOMPATIBLE_MEDIA'], [{ ...place, trackId: A1.id, position: 400, media: 'audio', linked: true }, 'INVALID_COMMAND'],
    [{ ...place, position: 400, audioTrackId: Muted.id }, 'TRACK_LOCKED'], [{ ...place, position: 400, audioTrackId: Titles.id }, 'INCOMPATIBLE_MEDIA'],
    [{ ...place, position: 400, allTracks: true }, 'INVALID_COMMAND'], [{ ...place, binId: colorAsset.id, position: 400, linked: true }, 'INCOMPATIBLE_MEDIA'],
    [{ ...place, trackId: Muted.id, position: 400, media: 'audio' }, 'TRACK_LOCKED']])
    assert.equal(await rejected('desktop_clip_insert', args), code, JSON.stringify(args));
  // Groups: ungroup unlinks the pair, regrouping its two halves links them again; groups nest.
  assert.deepEqual((await edit('desktop_clip_group', { clipIds: [pairV, pairA] })).changed, false);
  let grouping = await edit('desktop_clip_ungroup', { clipId: pairV });
  assert.deepEqual([grouping.clipIds, undoLabel(), clip(pairV).grouped, clip(pairV).linkedClipId], [[pairV, pairA].sort((a, b) => a - b), 'Ungroup clips', false, null]);
  grouping = await edit('desktop_clip_group', { clipIds: [pairV, pairA] });
  assert.deepEqual([grouping.linked, undoLabel(), clip(pairV).linkedClipId, clip(pairV).groupId], [true, 'Group clips', pairA, grouping.groupId]);
  grouping = await edit('desktop_clip_group', { clipIds: [pairV, colorClip] });
  assert.deepEqual([grouping.clipIds.length, clip(colorClip).groupId, clip(pairA).groupId, grouping.linked], [3, grouping.groupId, grouping.groupId, true]);
  grouping = await edit('desktop_clip_ungroup', { groupId: grouping.groupId });
  assert.deepEqual([grouping.clipIds.length, clip(colorClip).grouped, clip(pairV).linkedClipId], [3, false, pairA], 'Ungrouping the outer group keeps the linked pair');
  for (const [name, args, code] of [['desktop_clip_ungroup', { groupId: 999999 }, 'UNKNOWN_GROUP'], ['desktop_clip_ungroup', { clipId: colorClip }, 'UNKNOWN_GROUP'],
    ['desktop_clip_ungroup', { clipId: pairV, groupId: 1 }, 'INVALID_COMMAND'], ['desktop_clip_group', { clipIds: [pairV, 999999] }, 'UNKNOWN_CLIP']])
    assert.equal(await rejected(name, args), code, JSON.stringify(args));
  await edit('desktop_undo', { toIndex: timelineIndex }); assert.deepEqual(layout(), timelineStart);
  // Speed: the fixture pair plays at 0.5; normal speed halves its duration, on both linked halves, and Undo restores it.
  const fast = await edit('desktop_clip_speed', { clipId: pairV, speed: 1 });
  assert.deepEqual([fast.speed, fast.duration, fast.linkedClipId, undoLabel()], [1, 150, pairA, 'Change clip speed']);
  assert.deepEqual([clip(pairA).speed, clip(pairA).duration], [1, 150]);
  const speedEntry = await lastSummary();
  assert.deepEqual([speedEntry.summary.clipId, speedEntry.summary.speed, speedEntry.summary.previousDuration, speedEntry.summary.newDuration], [pairV, 1, 300, 150]);
  await edit('desktop_undo'); assert.deepEqual([clip(pairV).speed, clip(pairV).duration, clip(pairA).duration], [0.5, 300, 300]);
  await edit('desktop_clip_insert', { ...place, position: 320, linked: false });
  assert.equal(await rejected('desktop_clip_speed', { clipId: pairV, speed: 0.25 }), 'OVERLAP');
  await edit('desktop_undo', { toIndex: timelineIndex });
  for (const [args, code] of [[{ clipId: pairV, speed: 0 }, 'INVALID_COMMAND'], [{ clipId: colorClip, speed: 2 }, 'INCOMPATIBLE_MEDIA']])
    assert.equal(await rejected('desktop_clip_speed', args), code, JSON.stringify(args));
  // Enable and disable, with and without the linked partner.
  let toggled = await edit('desktop_clip_enable', { clipId: pairV, enabled: false });
  assert.deepEqual([toggled.clipIds, toggled.changed, clip(pairV).enabled, clip(pairA).enabled, undoLabel()], [[pairV, pairA].sort((a, b) => a - b), true, false, false, 'Disable clips']);
  assert.equal((await edit('desktop_clip_enable', { clipId: pairV, enabled: false })).changed, false);
  toggled = await edit('desktop_clip_enable', { clipId: pairV, enabled: true, linked: false });
  assert.deepEqual([toggled.clipIds, clip(pairV).enabled, clip(pairA).enabled, undoLabel()], [[pairV], true, false, 'Enable clips']);
  await edit('desktop_undo', { toIndex: timelineIndex });
  assert.deepEqual(layout(), timelineStart, 'Every timeline edit is undone');
  // Transcripts and silence. A 20 s tone with two silences goes on A1 at 600; transcripts in every import format go on the linked pair's
  // source, whose clips show source 10..15 s at speed 0.5 (sourceIn 600, so timeline frame = round(seconds * 60) - 600). Cuts by words,
  // silences, ranges and keep are each one Undo step. Everything is undone at the end of the section.
  await refresh();
  const transcriptCaps = await ok('desktop_capabilities');
  for (const operation of ['transcript_import', 'range_cut', 'transcribe']) assert(transcriptCaps.operations.includes(operation), operation);
  assert.deepEqual([transcriptCaps.transcriptFormats, transcriptCaps.transcriptOutputFormats, transcriptCaps.speechEngines],
    [['json', 'srt', 'vtt', 'whisper'], ['words', 'segments', 'text', 'srt'], ['whisper', 'vosk']]);
  const transcriptIndex = state.undo.index;
  const transcriptStart = layout();
  const guidesBefore = state.markers;
  const pairBin = sourceBin().id;
  const colorBin = state.bin.find(item => item.type === 'color').id;
  const sequenceBin = state.bin.find(item => item.type === 'sequence').id;
  const speechBin = (await edit('desktop_media_import', { path: speechFile })).binId;
  await ready(speechBin, speechFile);
  const speechClip = (await edit('desktop_clip_insert', { binId: speechBin, trackId: A1.id, position: 600, sourceIn: 0, sourceOut: 600, media: 'audio' })).clipId;
  assert.deepEqual([clip(speechClip).position, clip(speechClip).duration], [600, 600]);
  const cutBase = layout();
  const near = (actual, expected, label) => assert(Math.abs(actual - expected) <= 1, `${label}: ${actual} vs ${expected}`);
  const tail = () => lane('A1').clips.filter(item => item.position >= 570).map(item => [item.position, item.duration]);
  // Silence detection: the raw detection on the bin clip, padded silences mapped through the timeline clip, and a timeline range.
  let silence = await ok('desktop_silence_detect', { binId: speechBin, padding: 0 });
  assert.deepEqual([silence.target, silence.analyzed, silence.silences.length, silence.speech.length], ['bin', { start: 0, end: 20 }, 2, 3]);
  silence.silences.forEach((item, i) => {
    near(item.sourceStart, [90, 270][i], 'silence start'); near(item.sourceEnd, [150, 360][i], 'silence end');
    assert.equal(item.timelineStart, undefined);
  });
  silence = await ok('desktop_silence_detect', { clipId: speechClip });
  assert.deepEqual([silence.target, silence.thresholdDb, silence.minDuration, silence.padding], ['clip', -35, 0.5, 0.1]);
  silence.silences.forEach((item, i) => {
    near(item.timelineStart, [693, 873][i], 'padded start'); near(item.timelineEnd, [747, 957][i], 'padded end');
    near(item.detectedStart * 30, [90, 270][i], 'detected start'); assert.equal(item.clipId, speechClip);
  });
  const clipSilences = silence.silences.map(item => [item.timelineStart, item.timelineEnd]);
  assert.deepEqual(silence.speech.map(item => [item.timelineStart, item.timelineEnd]),
    [[600, clipSilences[0][0]], [clipSilences[0][1], clipSilences[1][0]], [clipSilences[1][1], 1200]]);
  const rangeSilence = await ok('desktop_silence_detect', { range: { start: 600, end: 1200 } });
  assert.deepEqual([rangeSilence.target, rangeSilence.clips.map(item => item.clipId)], ['range', [speechClip]]);
  assert.deepEqual(rangeSilence.silences.map(item => [item.timelineStart, item.timelineEnd]), clipSilences);
  // Frames with no clip at all are silent too.
  assert.deepEqual((await ok('desktop_silence_detect', { range: { start: 1150, end: 1300 } })).silences.map(item => [item.timelineStart, item.timelineEnd]), [[1200, 1300]]);
  for (const [args, code] of [[{ binId: colorBin }, 'NO_AUDIO'], [{ clipId: 999999 }, 'UNKNOWN_CLIP'], [{}, 'INVALID_ARGUMENTS'],
    [{ binId: speechBin, thresholdDb: 3 }, 'INVALID_ARGUMENTS'], [{ binId: speechBin, clipId: speechClip }, 'INVALID_ARGUMENTS'],
    [{ clipId: speechClip, sourceRange: { start: 0, end: 1 } }, 'INVALID_ARGUMENTS'], [{ binId: sequenceBin }, 'SEQUENCE_PROTECTED'],
    [{ range: { start: 1300, end: 1400 } }, 'NO_AUDIO']])
    assert.equal((await tool('desktop_silence_detect', args)).error.code, code, JSON.stringify(args));
  assert.equal((await ok('desktop_silence_detect', { binId: speechBin, sourceRange: { start: 8, end: 20 } })).silences.length, 1);
  // Transcript import (json) and readback by bin clip, timeline clip and range.
  // The fixture source has a transcript made in the Text-based edit panel (kdenlive:speech); an import replaces it and Undo brings it back.
  let readback = await ok('desktop_transcript', { binId: pairBin });
  assert.deepEqual([readback.origin, readback.engine, readback.words.map(word => [word.text, word.start, word.end, word.segment])],
    ['kdenlive:speech', 'kdenlive', [['Fixture', 10.5, 11, 0], ['speech.', 11.2, 11.6, 0]]]);
  const spoken = [['Before', 2, 2.5], ['Hello', 10.5, 11], ['world.', 11.2, 11.6], ['Um', 12, 12.5], ['second', 13.4, 14], ['take.', 14.1, 14.6]];
  const ownJson = { language: 'en', engine: 'test', words: spoken.map(([text, start, end]) => ({ start, end, text, confidence: 0.9 })) };
  let stored = await edit('desktop_transcript_import', { binId: pairBin, format: 'json', text: JSON.stringify(ownJson) });
  assert.deepEqual([stored.wordCount, stored.segmentCount, stored.language, stored.engine, stored.replaced, undoLabel()],
    [6, 4, 'en', 'test', true, 'Import transcript']);
  const importEntry = await lastSummary();
  assert.deepEqual([importEntry.command, importEntry.tool, importEntry.summary.binId, importEntry.summary.wordCount], ['transcript_import', 'desktop_transcript_import', pairBin, 6]);
  readback = await ok('desktop_transcript', { binId: pairBin });
  assert.deepEqual([readback.target, readback.origin, readback.language, readback.engine, readback.wordCount, readback.count], ['bin', 'mcp', 'en', 'test', 6, 6]);
  assert.deepEqual(readback.words.map(word => [word.index, word.text, word.start, word.end, word.sourceStart, word.sourceEnd, word.confidence, word.timelineStart]),
    spoken.map(([text, start, end], index) => [index, text, start, end, Math.round(start * 30), Math.round(end * 30), 0.9, undefined]));
  assert.deepEqual(readback.gaps.map(gap => [gap.start, gap.end, gap.afterWord, gap.beforeWord]), [[0, 2, -1, 0], [2.5, 10.5, 0, 1], [12.5, 13.4, 3, 4], [14.6, 20, 5, -1]]);
  // The words round-trip through our own json format.
  assert.deepEqual((await ok('desktop_transcript', { binId: pairBin, minPause: 5 })).gaps.map(gap => gap.start), [2.5, 14.6]);
  readback = await ok('desktop_transcript', { clipId: pairV });
  assert.deepEqual([readback.target, readback.clipId, readback.count], ['clip', pairV, 5]);
  assert.deepEqual(readback.words.map(word => [word.index, word.timelineStart, word.timelineEnd, word.clipId, word.trackId]),
    [[1, 30, 60, pairV, V1.id], [2, 72, 96, pairV, V1.id], [3, 120, 150, pairV, V1.id], [4, 204, 240, pairV, V1.id], [5, 246, 276, pairV, V1.id]]);
  assert.deepEqual(readback.gaps.map(gap => [gap.timelineStart, gap.timelineEnd, gap.afterWord, gap.beforeWord]), [[150, 204, 3, 4]]);
  readback = await ok('desktop_transcript', { clipId: pairV, format: 'segments' });
  assert.deepEqual(readback.segments.map(item => [item.text, item.firstWord, item.lastWord, item.timelineStart, item.timelineEnd]),
    [['Hello world.', 1, 2, 30, 96], ['Um', 3, 3, 120, 150], ['second take.', 4, 5, 204, 276]]);
  assert.equal(readback.words, undefined);
  assert.equal((await ok('desktop_transcript', { clipId: pairV, format: 'text' })).text, 'Hello world.\nUm\nsecond take.');
  assert.equal((await ok('desktop_transcript', { clipId: pairV, format: 'srt' })).text,
    '1\n00:00:01,000 --> 00:00:03,200\nHello world.\n\n2\n00:00:04,000 --> 00:00:05,000\nUm\n\n3\n00:00:06,800 --> 00:00:09,200\nsecond take.\n');
  readback = await ok('desktop_transcript', { range: { start: 100, end: 300 } });
  assert.deepEqual(readback.words.map(word => [word.index, word.clipId, word.binId, word.timelineStart]), [[3, pairA, pairBin, 120], [4, pairA, pairBin, 204], [5, pairA, pairBin, 246]]);
  assert.deepEqual(readback.clips.map(item => [item.clipId, item.wordCount]), [[pairA, 6]]);
  assert.deepEqual((await ok('desktop_transcript', { range: { start: 0, end: 100 }, trackIds: [V1.id] })).words.map(word => [word.clipId, word.index]), [[pairV, 1], [pairV, 2]]);
  assert.equal((await tool('desktop_transcript', { range: { start: 600, end: 700 } })).error.code, 'NO_TRANSCRIPT');
  for (const args of [{ binId: pairBin, format: 'html' }, { binId: pairBin, trackIds: [V1.id] }, { binId: pairBin, minPause: -1 }, { range: { start: 5, end: 5 } }])
    assert.equal((await tool('desktop_transcript', args)).error.code, 'INVALID_ARGUMENTS', JSON.stringify(args));
  // Cut words: extract (default) on every unlocked track; one Undo restores it.
  let rangeCut = await edit('desktop_range_cut', { clipId: pairV, words: [{ from: 3, to: 3 }] });
  assert.deepEqual([rangeCut.removedRanges, rangeCut.removedFrames, rangeCut.mode, rangeCut.source, rangeCut.newDuration, undoLabel()],
    [[{ start: 120, end: 150, duration: 30 }], 30, 'extract', 'words', 1170, 'Remove range']);
  assert.deepEqual([spans('V1'), spans('A1'), spans('Titles')], [[[0, 120], [120, 150]], [[0, 120], [120, 150], [570, 600]], [[30, 90], [120, 120]]]);
  assert(rangeCut.clipIdsAffected.includes(pairV) && rangeCut.clipIdsAffected.includes(speechClip) && rangeCut.newClipIds.length === 3, JSON.stringify(rangeCut));
  await edit('desktop_undo'); assert.deepEqual(layout(), cutBase);
  // keep: true removes everything but the given words inside the clip span; dryRun changes nothing.
  const historyBeforeDry = state.undo.index;
  rangeCut = await edit('desktop_range_cut', { clipId: pairV, words: [{ from: 1, to: 2 }, { from: 4, to: 5 }], keep: true, dryRun: true });
  assert.deepEqual([rangeCut.removedRanges.map(item => [item.start, item.end]), rangeCut.dryRun, rangeCut.changed, rangeCut.newDuration], [[[0, 30], [96, 204], [276, 300]], true, false, 1200 - 162]);
  assert.deepEqual([layout(), state.undo.index], [cutBase, historyBeforeDry]);
  // Ranges are merged, shrunk by padding and dropped below minGap.
  rangeCut = await edit('desktop_range_cut', { ranges: [{ start: 700, end: 710 }, { start: 705, end: 720 }, { start: 800, end: 803 }], padding: 1, minGap: 5, dryRun: true });
  assert.deepEqual(rangeCut.removedRanges.map(item => [item.start, item.end]), [[701, 719]]);
  // Cut every silence: the dry run matches silence detection; the real cut adds a guide at each join and is one Undo step.
  rangeCut = await edit('desktop_range_cut', { clipId: speechClip, silences: true, dryRun: true });
  assert.deepEqual(rangeCut.removedRanges.map(item => [item.start, item.end]), clipSilences);
  assert.deepEqual((await edit('desktop_range_cut', { range: { start: 600, end: 1200 }, silences: { padding: 0.1 }, dryRun: true })).removedRanges.map(item => [item.start, item.end]), clipSilences);
  const removedTotal = clipSilences.reduce((sum, [from, to]) => sum + to - from, 0);
  const beforeCut = state.undo.index;
  rangeCut = await edit('desktop_range_cut', { clipId: speechClip, silences: true, addMarkers: 3 });
  assert.deepEqual([rangeCut.removedFrames, rangeCut.newDuration, rangeCut.changed, undoLabel(), state.undo.index], [removedTotal, 1200 - removedTotal, true, 'Remove 2 ranges', beforeCut + 1]);
  const joins = [clipSilences[0][0], clipSilences[1][0] - (clipSilences[0][1] - clipSilences[0][0])];
  assert.deepEqual(rangeCut.markersAdded.map(item => [item.position, item.duration, item.category]), joins.map(position => [position, 0, 3]));
  assert.deepEqual(joins.map(position => guide(position)?.category), [3, 3]);
  assert.deepEqual(tail(), [[600, joins[0] - 600], [joins[0], joins[1] - joins[0]], [joins[1], 1200 - removedTotal - joins[1]]]);
  const cutEntry = await lastSummary();
  assert.deepEqual([cutEntry.command, cutEntry.tool, cutEntry.text, cutEntry.undoIndex, cutEntry.summary.source, cutEntry.summary.removedFrames, cutEntry.summary.ranges,
    cutEntry.summary.markersAdded, cutEntry.summary.removedRanges], ['range_cut', 'desktop_range_cut', 'Remove 2 ranges', beforeCut + 1, 'silences', removedTotal, 2, 2, clipSilences]);
  await edit('desktop_undo');
  assert.deepEqual([layout(), state.markers], [cutBase, guidesBefore], 'One Undo restores clips and guides');
  // Lift leaves gaps on the given tracks; keep with ranges inverts them inside range.
  rangeCut = await edit('desktop_range_cut', { ranges: [{ start: 650, end: 660 }], mode: 'lift', trackIds: [A1.id] });
  assert.deepEqual([rangeCut.newDuration, tail(), undoLabel()], [1200, [[600, 50], [660, 540]], 'Lift range']);
  await edit('desktop_undo');
  rangeCut = await edit('desktop_range_cut', { ranges: [{ start: 630, end: 690 }, { start: 750, end: 870 }], range: { start: 600, end: 1200 }, keep: true });
  assert.deepEqual([rangeCut.removedRanges.map(item => [item.start, item.end]), rangeCut.newDuration, tail()], [[[600, 630], [690, 750], [870, 1200]], 780, [[600, 60], [660, 120]]]);
  await edit('desktop_undo'); assert.deepEqual(layout(), cutBase);
  for (const [args, code] of [[{ clipId: speechClip, words: [{ from: 0, to: 0 }] }, 'NO_TRANSCRIPT'], [{ binId: pairBin, words: [{ from: 0, to: 9 }] }, 'INVALID_COMMAND'],
    [{ ranges: [{ start: 5, end: 5 }] }, 'INVALID_COMMAND'], [{ ranges: [{ start: 0, end: 5 }], silences: true }, 'INVALID_COMMAND'],
    [{ clipId: pairV, ranges: [{ start: 0, end: 5 }] }, 'INVALID_COMMAND'], [{ ranges: [{ start: 0, end: 5 }], trackIds: [Muted.id] }, 'TRACK_LOCKED'],
    [{ ranges: [{ start: 0, end: 5 }], addMarkers: 'Nope' }, 'UNKNOWN_CATEGORY'], [{ range: { start: 0, end: 10 }, words: [{ from: 0, to: 0 }] }, 'INVALID_COMMAND'],
    [{ binId: state.bin.find(item => item.type === 'title').id, silences: true }, 'UNKNOWN_CLIP'], [{ clipId: colorClip, silences: true }, 'NO_AUDIO'],
    [{ range: { start: 0, end: 300 }, silences: true, trackIds: [V1.id] }, 'NO_AUDIO']])
    assert.equal(await rejected('desktop_range_cut', args), code, JSON.stringify(args));
  // srt and vtt give cue timing, split equally between the words; whisper keeps its word timing and probabilities.
  const srtText = '1\n00:00:10,500 --> 00:00:11,600\nHello <i>world</i>.\n\n2\n00:00:13,400 --> 00:00:14,600\nsecond take.\n';
  stored = await edit('desktop_transcript_import', { binId: pairBin, format: 'srt', text: srtText, language: 'en' });
  assert.deepEqual([stored.wordCount, stored.segmentCount, stored.interpolatedWords, stored.replaced, stored.engine, stored.language], [4, 2, 4, true, 'import', 'en']);
  assert.deepEqual((await ok('desktop_transcript', { binId: pairBin })).words.map(word => [word.text, word.start, word.end, word.segment, word.interpolated]),
    [['Hello', 10.5, 11.05, 0, true], ['world.', 11.05, 11.6, 0, true], ['second', 13.4, 14, 1, true], ['take.', 14, 14.6, 1, true]]);
  assert.deepEqual((await ok('desktop_transcript', { clipId: pairV, format: 'segments' })).segments.map(item => [item.text, item.timelineStart, item.timelineEnd]),
    [['Hello world.', 30, 96], ['second take.', 204, 276]]);
  stored = await edit('desktop_transcript_import', { binId: pairBin, format: 'vtt', text: 'WEBVTT\n\nNOTE made by hand\n\n00:10.500 --> 00:11.600 align:start\nHello world.\n' });
  assert.deepEqual((await ok('desktop_transcript', { binId: pairBin })).words.map(word => [word.text, word.start, word.end]), [['Hello', 10.5, 11.05], ['world.', 11.05, 11.6]]);
  const whisper = { text: ' Hello world. Second take.', language: 'en', segments: [
    { id: 0, start: 10.5, end: 11.6, text: ' Hello world.', words: [{ word: ' Hello', start: 10.5, end: 11, probability: 0.75 }, { word: ' world.', start: 11.2, end: 11.6, probability: 0.5 }] },
    { id: 1, start: 13.4, end: 14.6, text: ' Second take.' }] };
  stored = await edit('desktop_transcript_import', { binId: pairBin, format: 'whisper', text: JSON.stringify(whisper) });
  assert.deepEqual([stored.wordCount, stored.language, stored.engine, stored.interpolatedWords], [4, 'en', 'whisper', 2]);
  assert.deepEqual((await ok('desktop_transcript', { clipId: pairV })).words.map(word => [word.text, word.timelineStart, word.timelineEnd, word.confidence ?? null, word.segment]),
    [['Hello', 30, 60, 0.75, 0], ['world.', 72, 96, 0.5, 0], ['Second', 204, 240, null, 1], ['take.', 240, 276, null, 1]]);
  const beforeAppend = state.undo.index;
  stored = await edit('desktop_transcript_import', { binId: pairBin, format: 'json', text: JSON.stringify([{ start: 16, end: 16.5, text: 'Later' }]), append: true });
  assert.deepEqual([stored.wordCount, stored.appended, stored.replaced, state.undo.index], [5, true, false, beforeAppend + 1]);
  await edit('desktop_undo'); assert.equal((await ok('desktop_transcript', { binId: pairBin })).wordCount, 4, 'Each import is its own Undo step');
  for (const [args, code] of [[{ binId: pairBin, format: 'json', text: '{"words":[{"start":2,"end":1,"text":"x"}]}' }, 'INVALID_TRANSCRIPT'],
    [{ binId: pairBin, format: 'srt', text: 'not subtitles' }, 'INVALID_TRANSCRIPT'], [{ binId: pairBin, format: 'json', text: '[{"start":300,"end":301,"text":"late"}]' }, 'INVALID_TRANSCRIPT'],
    [{ binId: pairBin, format: 'whisper', text: '{"text":"no segments"}' }, 'INVALID_TRANSCRIPT'], [{ binId: pairBin, format: 'json', text: '[]' }, 'INVALID_TRANSCRIPT'],
    [{ binId: colorBin, format: 'json', text: '[]' }, 'NO_AUDIO'], [{ binId: sequenceBin, format: 'json', text: '[]' }, 'SEQUENCE_PROTECTED'],
    [{ binId: pairBin, format: 'docx', text: 'x' }, 'INVALID_COMMAND']])
    assert.equal(await rejected('desktop_transcript_import', args), code, JSON.stringify(args));
  // Batch: a transcript import and a cut in one Undo step.
  const beforeTranscriptBatch = state.undo.index;
  const batched = await edit('desktop_batch', { commands: [
    { type: 'transcript_import', binId: speechBin, format: 'srt', text: '1\n00:00:00,500 --> 00:00:02,000\nTone one\n' },
    { type: 'range_cut', ranges: [{ start: 650, end: 660 }], mode: 'lift', trackIds: [A1.id] }] });
  assert.deepEqual([batched.results.map(item => item.ok), state.undo.index, undoLabel()], [[true, true], beforeTranscriptBatch + 1, 'MCP batch (2 edits)']);
  assert.deepEqual((await ok('desktop_transcript', { clipId: speechClip })).words.map(word => [word.text, word.timelineStart]), [['Tone', 615], ['one', 638]]);
  await edit('desktop_undo');
  assert.equal((await tool('desktop_transcript', { binId: speechBin })).error.code, 'NO_TRANSCRIPT');
  assert.deepEqual(layout(), cutBase);
  // Live transcription needs Kdenlive's speech-to-text Python environment and a model; without them the tool reports what is missing.
  for (const engine of ['whisper', 'vosk']) {
    await refresh();
    const started = await tool('desktop_transcribe', { ...fields(), binId: speechBin, engine });
    if (started.ok) {
      let job;
      for (let attempt = 0; attempt < 6000 && job?.state !== 'finished' && job?.state !== 'failed'; attempt++) {
        job = (await ok('desktop_transcribe_status', { jobId: started.data.jobId })).jobs[0];
        await delay(100);
      }
      assert.equal(job.state, 'finished', JSON.stringify(job));
      console.log(`desktop_transcribe (${engine}) ran: ${job.wordCount} words`);
      continue;
    }
    assert.equal(started.error.code, 'TRANSCRIPTION_UNAVAILABLE', JSON.stringify(started));
    assert.deepEqual([typeof started.reason, started.engine, Array.isArray(started.installedModels), Array.isArray(started.missingDependencies), typeof started.hint],
      ['string', engine, true, true, 'string'], JSON.stringify(started));
    console.log(`desktop_transcribe (${engine}) unavailable here: ${started.reason}`);
  }
  assert.deepEqual((await ok('desktop_transcribe_status')).jobs.filter(job => job.state === 'running'), []);
  assert.equal((await tool('desktop_transcribe_status', { jobId: 'nope' })).error.code, 'UNKNOWN_JOB');
  assert.equal(await rejected('desktop_transcribe', { binId: colorBin }), 'NO_AUDIO');
  assert.equal(await rejected('desktop_transcribe', { binId: speechBin, engine: 'siri' }), 'INVALID_COMMAND');
  await edit('desktop_undo', { toIndex: transcriptIndex });
  assert.deepEqual([layout(), state.markers], [transcriptStart, guidesBefore], 'Every transcript edit is undone');
  assert(!state.bin.some(item => item.id === speechBin));
  assert.equal((await ok('desktop_transcript', { binId: pairBin })).origin, 'kdenlive:speech', 'Undo restores the panel transcript');
  // Replacing a clip's media reloads it from the new file, which drops its transcript (checked below).
  await edit('desktop_transcript_import', { binId: pairBin, format: 'json', text: JSON.stringify(ownJson) });
  const video = state.tracks.find(item => !item.audio && !item.locked);
  const audio = state.tracks.find(item => item.audio && item.clips.length);
  const asset = state.bin.find(item => item.ready && item.url === source);
  assert(video && audio && asset);
  const insertion = { ...fields(), binId: asset.id, trackId: video.id, position: 360, sourceIn: 30, sourceOut: 90, media: 'video', linked: false };
  const inserted = await ok('desktop_clip_insert', insertion);
  assert.deepEqual(Object.keys(inserted.state).sort(), Object.keys(state).sort(), 'Edits embed the default desktop_state snapshot');
  state = inserted.state;
  const id = inserted.clipId;
  assert.deepEqual([clip(id).grouped, clip(id).groupId, clip(id).linkedClipId, clip(id).enabled], [false, null, null, true]);
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
  assert.equal((await tool('desktop_transcript', { binId: asset.id })).error.code, 'NO_TRANSCRIPT', 'Replaced media has no transcript');
  assert.equal(clip(id).position, 420); assert.equal(clip(id).duration, 45);
  await edit('desktop_undo'); await ready(asset.id, source);
  await edit('desktop_redo'); await ready(asset.id, replacement);
  await edit('desktop_media_remove', { binId: imported.binId });
  // Replaying an already applied replacement must not re-check its removed source bin item.
  assert.deepEqual(await ok('desktop_media_replace', replaceRequest), replaced);
  // A transcript is a bin clip property: it is saved with the project and read back after a reload.
  await edit('desktop_transcript_import', { binId: asset.id, format: 'json', text: JSON.stringify(ownJson) });
  await edit('desktop_project_save'); assert.equal(state.modified, false);
  assert.match(await readFile(project, 'utf8'), /name="kdenlive_id">volume<\/property>/);
  assert.match(await readFile(project, 'utf8'), /name="kdenlive:mcp_transcript"/);
  const previousSession = state.sessionId;
  await stop(); await launch();
  assert.notEqual(state.sessionId, previousSession);
  assert(state.tracks.flatMap(track => track.clips).some(item => item.position === 420 && item.duration === 45 && item.sourceIn === 30));
  assert.equal(state.tracks.find(track => track.name === 'MCP Music').clips[0].effectCount, 1);
  const reloaded = await ok('desktop_transcript', { binId: state.bin.find(item => item.url === replacement).id });
  assert.deepEqual([reloaded.origin, reloaded.language, reloaded.words.map(word => word.text)], ['mcp', 'en', ['Before', 'Hello', 'world.', 'Um', 'second', 'take.']]);
  assert.equal((await tool('desktop_clip_insert', insertion)).error.code, 'SESSION_CHANGED');

  // Change log: entries made through MCP carry their request, tool, connection and affected ids; undo and redo move them between applied
  // and undone; revertSession undoes one connection's edits unless another connection's edit lies in between.
  await refresh();
  assert.equal(state.lastChange, null, 'A restarted editor starts a new history');
  const historyStart = state.undo.index;
  const historyTrack = state.tracks.find(track => !track.audio && !track.locked);
  const h1 = await edit('desktop_marker_add', { position: 33, comment: 'History one' });
  const h2 = await edit('desktop_track_rename', { trackId: historyTrack.id, name: 'History V' });
  const h3 = await edit('desktop_batch', { commands: [{ type: 'marker_add', position: 34, comment: 'History batch' }, { type: 'marker_edit', position: 33, comment: 'History one, edited' }] });
  assert.deepEqual(state.lastChange, { undoIndex: historyStart + 3, command: 'batch', text: 'MCP batch (2 edits)', requestId: h3.requestId, applied: true });
  let changes = await ok('desktop_history', { since: historyStart });
  assert.deepEqual(changes.entries.map(entry => [entry.undoIndex, entry.origin, entry.command, entry.tool, entry.state, entry.requestId, entry.text]), [
    [historyStart + 1, 'mcp', 'marker_add', 'desktop_marker_add', 'applied', h1.requestId, 'Add guide'],
    [historyStart + 2, 'mcp', 'rename_track', 'desktop_track_rename', 'applied', h2.requestId, 'Rename Track'],
    [historyStart + 3, 'mcp', 'batch', 'desktop_batch', 'applied', h3.requestId, 'MCP batch (2 edits)']]);
  const [e1, e2, e3] = changes.entries;
  assert.match(e1.rawText, /^\d\d:\d\d Add guide$/); assert.equal(e3.rawText, 'MCP batch (2 edits)');
  assert.deepEqual(e1.summary, { position: 33, replaced: false, marker: { position: 33, duration: 0 } });
  assert.deepEqual(e2.summary, { trackId: historyTrack.id, name: 'History V' });
  assert.deepEqual([e3.summary, e3.children], [{ commands: 2 }, [
    { type: 'marker_add', summary: { position: 34, replaced: false, marker: { position: 34, duration: 0 } } },
    { type: 'marker_edit', summary: { position: 33, marker: { position: 33, duration: 0 } } }]]);
  assert.equal(e1.children, undefined);
  assert(h3.results.every(result => result.historySummary === undefined));
  for (const entry of changes.entries) {
    assert.deepEqual([entry.sessionId, entry.clientName, entry.client], [state.sessionId, 'native-acceptance', e1.client]);
    assert(entry.revisionAfter > entry.revisionBefore, JSON.stringify(entry));
    assert(!Number.isNaN(Date.parse(entry.timestamp)) && entry.timestamp.endsWith('Z'), entry.timestamp);
  }
  assert.match(e1.client, /^[0-9a-f]{12}$/);
  assert.equal(e3.revisionAfter, state.revision);
  assert.deepEqual([changes.undo.index, changes.matched, changes.counts.applied, changes.counts.undone], [historyStart + 3, 3, historyStart + 3, 0]);
  assert.equal(changes.counts.mcp + changes.counts.user + changes.counts.unknown, historyStart + 3);
  assert.equal(changes.logFile, join(root, '.kdenlive-mcp-log.jsonl'));
  const twoBack = await edit('desktop_undo', { count: 2 });
  assert.deepEqual([twoBack.steps, twoBack.undoIndex, twoBack.undone.map(entry => [entry.undoIndex, entry.state])],
    [2, historyStart + 1, [[historyStart + 3, 'undone'], [historyStart + 2, 'undone']]]);
  assert.deepEqual([guide(34), guide(33).comment, state.tracks.find(track => track.id === historyTrack.id).name], [undefined, 'History one', historyTrack.name]);
  assert.deepEqual([state.lastChange.undoIndex, state.lastChange.applied], [historyStart + 3, false]);
  changes = await ok('desktop_history', { since: historyStart });
  assert.deepEqual([changes.entries.map(entry => entry.state), changes.counts.undone], [['applied', 'undone', 'undone'], 2]);
  assert.deepEqual((await ok('desktop_history', { since: historyStart, includeUndone: false })).entries.map(entry => entry.undoIndex), [historyStart + 1]);
  const forward = await edit('desktop_redo', { toIndex: historyStart + 3 });
  assert.deepEqual([forward.steps, forward.redone.map(entry => [entry.undoIndex, entry.state])], [2, [[historyStart + 2, 'applied'], [historyStart + 3, 'applied']]]);
  assert.deepEqual([guide(34).comment, guide(33).comment], ['History batch', 'History one, edited']);
  await edit('desktop_undo', { toIndex: historyStart + 2 }); assert.equal(guide(34), undefined);
  await edit('desktop_redo'); assert.equal(guide(34).comment, 'History batch');
  assert.deepEqual((await ok('desktop_history', { since: historyStart, limit: 1 })).entries.map(entry => entry.command), ['batch']);
  const sinceTime = await ok('desktop_history', { since: e2.timestamp });
  assert(sinceTime.entries.every(entry => entry.timestamp >= e2.timestamp) && sinceTime.entries.some(entry => entry.requestId === h3.requestId));
  assert.deepEqual((await ok('desktop_history', { sessionId: randomUUID() })).entries, []);
  assert.deepEqual((await ok('desktop_history', { origin: 'mcp', since: historyStart })).entries.length, 3);
  assert((await ok('desktop_history', { origin: 'user' })).entries.every(entry => entry.origin === 'user' && entry.requestId === undefined));
  for (const args of [{ limit: 0 }, { limit: 1001 }, { origin: 'robot' }, { since: 'yesterday' }, { since: -1 }, { includeUndone: 'yes' }, { extra: 1 }])
    assert.equal((await tool('desktop_history', args)).error.code, 'INVALID_ARGUMENTS', JSON.stringify(args));
  assert.equal(await rejected('desktop_undo', { count: state.undo.index + 1 }), 'EMPTY_HISTORY');
  assert.equal(await rejected('desktop_redo', { count: 1 }), 'EMPTY_HISTORY');
  assert.equal(await rejected('desktop_undo', { count: 1, toIndex: 0 }), 'INVALID_COMMAND');
  assert.equal(await rejected('desktop_undo', { revertSession: false }), 'INVALID_COMMAND');
  assert.equal(await rejected('desktop_redo', { revertSession: true }), 'INVALID_COMMAND');
  assert.equal(await rejected('desktop_undo', { toIndex: state.undo.index + 1 }), 'INVALID_ARGUMENTS');
  assert.equal(await rejected('desktop_redo', { toIndex: 0 }), 'INVALID_ARGUMENTS');
  const withHistory = await ok('desktop_state', { include: ['history'] });
  assert.deepEqual([withHistory.sections, withHistory.tracks], [['history'], undefined]);
  assert(withHistory.history.length <= 10);
  assert.deepEqual(withHistory.history.at(-1), (await ok('desktop_history', { limit: 1 })).entries[0]);
  // A second MCP connection stands in for another editor: the offscreen test editor has no GUI input, so no user-origin entry can be made.
  const second = await connect('second-agent');
  await refresh();
  const other = (await second.client.callTool({ name: 'desktop_marker_add', arguments: { ...fields(), position: 35, comment: 'Other agent' } })).structuredContent;
  assert.equal(other.ok, true, JSON.stringify(other));
  const otherEntry = (await ok('desktop_history', { limit: 1 })).entries[0];
  assert.deepEqual([otherEntry.requestId, otherEntry.clientName, otherEntry.origin, otherEntry.undoIndex], [other.data.requestId, 'second-agent', 'mcp', historyStart + 4]);
  assert.notEqual(otherEntry.client, e1.client);
  await refresh();
  const interleaved = await tool('desktop_undo', { ...fields(), revertSession: true });
  assert.deepEqual([interleaved.ok, interleaved.error.code, interleaved.toIndex], [false, 'HISTORY_INTERLEAVED', historyStart]);
  assert.deepEqual(interleaved.blocking.map(entry => [entry.undoIndex, entry.requestId]), [[historyStart + 4, other.data.requestId]]);
  await refresh(); assert.equal(guide(35).comment, 'Other agent', 'A refused revert changes nothing');
  const otherRevert = (await second.client.callTool({ name: 'desktop_undo', arguments: { ...fields(), revertSession: true } })).structuredContent;
  assert.deepEqual([otherRevert.ok, otherRevert.data?.steps, otherRevert.data?.undone.map(entry => entry.requestId)], [true, 1, [other.data.requestId]]);
  await second.transport.terminateSession().catch(() => {}); await second.client.close();
  const reverted = await edit('desktop_undo', { revertSession: true });
  assert.deepEqual([reverted.steps, reverted.undoIndex, reverted.undone.map(entry => entry.requestId)], [3, historyStart, [h3.requestId, h2.requestId, h1.requestId]]);
  assert.deepEqual([guide(33), guide(34), guide(35), state.tracks.find(track => track.id === historyTrack.id).name], [undefined, undefined, undefined, historyTrack.name]);
  assert.equal(await rejected('desktop_undo', { revertSession: true }), 'EMPTY_HISTORY');
  // The JSON lines log next to the project keeps both editor sessions.
  const logLines = (await readFile(join(root, '.kdenlive-mcp-log.jsonl'), 'utf8')).trimEnd().split('\n').map(line => JSON.parse(line));
  const h1Line = logLines.find(line => line.requestId === h1.requestId);
  assert.deepEqual([h1Line.event, h1Line.command, h1Line.tool, h1Line.text, h1Line.undoIndex, h1Line.sessionId, h1Line.documentUrl, h1Line.clientName, h1Line.summary],
    ['edit', 'marker_add', 'desktop_marker_add', 'Add guide', historyStart + 1, state.sessionId, state.documentUrl, 'native-acceptance', e1.summary]);
  assert(h1Line.revisionAfter > h1Line.revisionBefore && !Number.isNaN(Date.parse(h1Line.timestamp)));
  assert.deepEqual(logLines.find(line => line.requestId === h3.requestId).children, e3.children);
  assert.equal(logLines.find(line => line.requestId === other.data.requestId).clientName, 'second-agent');
  const revertLine = logLines.find(line => line.requestId === reverted.requestId);
  assert.deepEqual([revertLine.event, revertLine.summary.revertSession, revertLine.summary.steps, revertLine.entries.map(entry => entry.undoIndex)],
    ['undo', true, 3, [historyStart + 3, historyStart + 2, historyStart + 1]]);
  assert(logLines.some(line => line.sessionId === previousSession && line.event === 'edit' && line.command === 'insert'), 'First editor session is logged');
  assert(logLines.some(line => line.sessionId === previousSession && line.event === 'save'));
  assert(!logLines.some(line => line.requestId === undefined || line.documentUrl !== pathToFileURL(project).href), 'Every line names the project');
  // Tools for reworking a project's format: capabilities, capture, effects, titles, batch, save-as, profile, reframe, render.
  const capabilities = await ok('desktop_capabilities');
  for (const operation of ['save_as', 'set_profile', 'reframe', 'effect_add', 'effect_set', 'effect_remove', 'title_edit', 'render', 'batch'])
    assert(capabilities.operations.includes(operation), `capabilities lacks ${operation}`);
  assert.deepEqual(capabilities.defaultStateSections, state.sections);
  assert.deepEqual(capabilities.stateSections, ['tracks', 'clips', 'compositions', 'markers', 'subtitles', 'bin', 'sequences', 'effects', 'media', 'history']);
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
  const summary = await ok('desktop_state', { include: ['effects'], trackId: videoTrack.id });
  assert.deepEqual(summary.sections, ['tracks', 'clips', 'effects']);
  const summarized = summary.tracks[0].clips.find(item => item.id === clipId);
  assert.equal(summarized.effects.length, summarized.effectCount);
  assert.deepEqual(summarized.effects[added.effectIndex], { effectId: 'brightness', name: brightness.name, enabled: true });
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
  const finalHistory = await ok('desktop_history', { limit: 1000 });
  assert.equal(finalHistory.logFile, join(root, '.kdenlive-mcp-log.jsonl'), 'save_as keeps logging in the shared project folder');
  await writeFile(join(root, 'report.json'), JSON.stringify({ passed: true, transport: 'native Streamable HTTP', legacyBridge: false, tools: catalog.tools.map(tool => tool.name), timings, history: finalHistory, finalState: state }, null, 2));
  console.log(`Native MCP acceptance passed: ${root}/report.json`);
} catch (error) {
  console.error(`Failed after: ${timings.slice(-5).map(item => item.tool).join(' -> ')}; logs in ${root}`);
  throw error;
} finally { await stop(); }
