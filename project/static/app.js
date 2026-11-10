import { Api, el } from './js/api.js?v=dash-a-2';
import { CloudJobs } from './js/cloud.js?v=dash-a-2';
import { AudioPlayer } from './js/audio.js?v=dash-a-2';
import { CameraController } from './js/camera.js?v=dash-a-2';
import { UploadController } from './js/upload.js?v=dash-a-2';
import { DeviceController } from './js/device.js?v=dash-a-2';
import { watchEvents } from './js/events.js?v=dash-a-2';
import { StatsView } from './js/stats.js?v=dash-a-2';

const api = new Api();
let current = null,
  mode = 'upload',
  localOnly = false,
  renderVersion = 0,
  photoURL = null,
  streamAbort = null;
const seen = new Set();
const error = (message) => {
  el('error').textContent = message;
};
const jobs = new CloudJobs(api, (message) => {
  el('camera-status').textContent = message;
  el('processing').textContent = message;
  el('processing').hidden = false;
});
const gestureName = (event) =>
  event.finger_count
    ? `${event.finger_count} ngón`
    : event.partial && event.visible_finger_count
      ? `Ít nhất ${event.visible_finger_count} ngón`
      : event.gesture === 'No_hand'
        ? 'Không thấy tay'
        : 'Chưa rõ';
const known = (event) => Boolean(event.finger_count);
const sourceName = (event) =>
  event.source === 'upload' ? 'Tải ảnh' : event.source === 'browser' ? 'Camera máy này' : 'ESP32';
const clock = (time) =>
  new Date(time || Date.now()).toLocaleTimeString('vi-VN', { hour: '2-digit', minute: '2-digit' });

function row(event) {
  if ((!event.confirmed && event.pipeline_version === 6) || seen.has(event.id)) return;
  seen.add(event.id);
  const li = document.createElement('li');
  li.dataset.id = event.id;
  li.title = event.text || '';
  const time = document.createElement('span'),
    name = document.createElement('strong'),
    from = document.createElement('span');
  time.textContent = clock(event.time);
  name.textContent = gestureName(event);
  if (!known(event)) name.className = 'unknown';
  from.textContent = sourceName(event);
  li.append(time, name, from);
  el('rows').prepend(li);
  while (el('rows').children.length > 50) {
    seen.delete(el('rows').lastChild.dataset.id);
    el('rows').lastChild.remove();
  }
  el('history-empty').hidden = true;
}
const acknowledge = (event, played) => {
  if (!event?.cloud || event.device_id) return;
  api.post('/api/jobs/' + event.id + '/ack', { displayed: true, played }).catch(() => {});
};
const audio = new AudioPlayer(api, error, (event) => acknowledge(event, true));

function holdIdle() {
  el('hold-progress').value = 0;
  el('hold-status').textContent = '';
}
function clear() {
  holdIdle();
  el('camera-actions').hidden = false;
  el('phone-manual-controls').hidden =
    el('phone-mode').value !== 'manual' || el('camera-source').value === 'cloud';
  current = null;
  renderVersion++;
  audio.stop();
  error('');
  el('gesture').textContent = '—';
  el('gesture').classList.remove('unknown');
  el('phrase').textContent =
    mode === 'upload'
      ? 'Tải ảnh bàn tay lên rồi bấm “Gửi nhận diện”.'
      : 'Đưa tay vào khung camera để nhận diện.';
  el('speak').disabled = true;
  el('resume-round').hidden = true;
  el('duration').textContent = 'Chưa có lần nhận diện nào.';
  for (const id of ['hand-summary', 'speech-status', 'confidence']) el(id).textContent = '';
  draw();
}
function draw() {
  const photo = el('photo'),
    canvas = el('marks');
  canvas.width = photo.clientWidth;
  canvas.height = photo.clientHeight;
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  if (current && photo.naturalWidth) {
    const scale = Math.min(canvas.width / photo.naturalWidth, canvas.height / photo.naturalHeight),
      w = photo.naturalWidth * scale,
      h = photo.naturalHeight * scale;
    const ox = (canvas.width - w) / 2,
      oy = (canvas.height - h) / 2;
    ctx.fillStyle = '#38d6c9';
    for (const hand of current.hands || [])
      for (const point of hand.landmarks || []) {
        ctx.beginPath();
        ctx.arc(ox + point.x * w, oy + point.y * h, 3, 0, 2 * Math.PI);
        ctx.fill();
      }
  }
  if (mode === 'upload') upload.draw();
}
function showPhoto(url) {
  el('photo').onload = draw;
  el('photo').src = url;
  el('photo').hidden = false;
  el('placeholder').hidden = true;
  el('live-preview').hidden = true;
}
async function image(url) {
  const version = renderVersion;
  const blob = await (await api.request(url)).blob();
  if (version !== renderVersion) return;
  const next = URL.createObjectURL(blob);
  if (photoURL) URL.revokeObjectURL(photoURL);
  photoURL = next;
  showPhoto(next);
}
async function result(event, speak = true) {
  if (!event.confirmed) return;
  error('');
  const version = ++renderVersion;
  current = event;
  row(event);
  el('processing').hidden = true;
  el('gesture').textContent = gestureName(event);
  el('gesture').classList.toggle('unknown', !known(event));
  el('phrase').textContent = event.text;
  el('speak').disabled = !event.audio_url;
  const seconds = ((event.latency_ms || 0) / 1000).toLocaleString('vi-VN', {
    maximumFractionDigits: 1,
  });
  el('duration').textContent =
    `${sourceName(event)} · xử lý ${seconds} s · ${event.frames_checked || 1} ảnh · ${clock(event.time)}`;
  el('hand-summary').textContent = (event.hands || [])
    .map((h, i) => `Tay ${i + 1}: ${h.finger_count ?? 'chưa rõ'} ngón`)
    .join(' · ');
  el('confidence').textContent = 'Cách nhận diện: điểm bàn tay MediaPipe';
  el('speech-status').textContent = event.audio_url
    ? 'Giọng đọc từ clip đã thu.'
    : 'Chưa có clip âm thanh.';
  el('resume-round').hidden = mode !== 'camera';
  if (mode === 'camera') {
    holdIdle();
    el('camera-actions').hidden = true;
    el('phone-manual-controls').hidden = true;
  }
  try {
    await image(event.preview_url || '/api/jobs/' + event.id + '/preview.jpg');
  } catch (e) {
    error(e.message);
  }
  if (version !== renderVersion) return;
  acknowledge(event, false);
  if (speak && el('auto').checked && event.audio_url) await audio.play(event, true);
}
const camera = new CameraController(jobs, {
  start: () => clear(),
  status: (message) => {
    el('camera-status').textContent = message;
  },
  error,
  progress: (seconds) => {
    el('hold-progress').value = seconds;
    const left = Math.max(0, 2 - seconds).toLocaleString('vi-VN', {
      minimumFractionDigits: 1,
      maximumFractionDigits: 1,
    });
    el('hold-status').textContent = `Tự chụp sau ${left} s`;
  },
  preview: (url) => {
    clear();
    if (photoURL) URL.revokeObjectURL(photoURL);
    photoURL = url;
    showPhoto(url);
  },
  result,
});
function busy(value) {
  el('upload-button').disabled = value || !el('file').files[0];
  el('upload-clear').disabled = value || !el('file').files[0];
  el('file').disabled = value;
  el('processing').hidden = !value;
  if (value) audio.stop();
}
const upload = new UploadController(jobs, {
  error,
  busy,
  result,
  redraw: draw,
  preview: (url) => {
    clear();
    showPhoto(url);
  },
});
const device = new DeviceController(api, { result, image });
const stats = new StatsView(api);
function showStats(open) {
  document.querySelector('main').hidden = open;
  history.replaceState(null, '', open ? '#bieu-do' : location.pathname + location.search);
  if (open) {
    camera.stop();
    stats.show();
    el('stats-close').focus();
  } else {
    stats.hide();
    el('stats-open').focus();
  }
}

// ----- Nguon hinh anh: ESP32 / camera may nay / tai anh -----
function currentSource() {
  if (mode === 'upload') return 'upload';
  return el('camera-source').value === 'cloud' ? 'esp32' : 'phone';
}
function syncSource() {
  const name = currentSource();
  el('preview-panel').dataset.source = name;
  for (const key of ['esp32', 'phone', 'upload'])
    el('src-' + key).setAttribute('aria-pressed', String(key === name));
  if (name === 'phone') syncMode(el('phone-mode').value);
}
function syncMode(value) {
  el('mode-auto').setAttribute('aria-pressed', String(value === 'auto'));
  el('mode-manual').setAttribute('aria-pressed', String(value === 'manual'));
}
function source() {
  camera.stop();
  const hardware = el('camera-source').value === 'cloud';
  el('phone-manual-controls').hidden = hardware || el('phone-mode').value !== 'manual';
  syncSource();
  device.activate(mode === 'camera' && hardware);
}
function setMode(next) {
  if (mode === next) return;
  upload.cancel();
  camera.stop();
  device.activate(false);
  mode = next;
  clear();
  el('photo').hidden = true;
  el('file-info').hidden = true;
  el('placeholder').hidden = false;
  el('placeholder').textContent =
    mode === 'upload' ? 'Chưa có ảnh. Bấm “Tải ảnh lên”.' : 'Đang chờ hình từ camera…';
  if (mode === 'camera') source();
  else syncSource();
}
function pickSource(name) {
  if (name === 'upload') return setMode('upload');
  const value = name === 'esp32' ? 'cloud' : 'device';
  const changed = el('camera-source').value !== value;
  el('camera-source').value = value;
  if (mode !== 'camera') setMode('camera');
  else if (changed) {
    clear();
    source();
  }
  el('placeholder').textContent =
    name === 'esp32' ? 'Đang chờ hình từ ESP32…' : 'Bấm “Bật camera” để bắt đầu.';
}
function pickMode(value) {
  if (currentSource() === 'esp32') {
    syncMode(value);
    device.command(value);
    return;
  }
  el('phone-mode').value = value;
  syncMode(value);
  camera.stop();
  clear();
}

async function connect() {
  camera.stop();
  upload.cancel();
  streamAbort?.abort();
  api.key = el('key').value.trim();
  try {
    const [history, diagnostics] = await Promise.all([
      api.json('/api/history'),
      api.json('/api/diagnostics'),
    ]);
    seen.clear();
    el('rows').replaceChildren();
    history.reverse().forEach(row);
    const cloudReady = diagnostics.cloud?.connected;
    el('connection').textContent = localOnly
      ? '● Nhận diện local trên máy này'
      : cloudReady
        ? '● Đã kết nối Pi + Supabase'
        : '◐ Đã xác thực Pi · đang kết nối Supabase…';
    el('settings-status').textContent = localOnly
      ? 'Đang chạy local, không cần API key.'
      : cloudReady
        ? 'Đã xác thực dashboard và Supabase.'
        : 'Đã xác thực dashboard. Supabase đang khởi tạo, thử gửi lại sau ít giây.';
    el('settings-dialog').close();
    error('');
    streamAbort = new AbortController();
    watchEvents(
      api,
      (event) => {
        row(event);
        stats.onEvent();
        if (event.device_id) device.poll();
      },
      streamAbort.signal,
    );
    if (mode === 'camera') source();
  } catch (e) {
    el('settings-status').textContent = e.message;
    error(e.message);
  }
}

// ----- Su kien -----
el('form').onsubmit = (event) => upload.submit(event);
el('file').onchange = () => {
  upload.choose();
  const file = el('file').files[0];
  el('upload-button').disabled = !file;
  el('upload-clear').disabled = !file;
  el('file-info').hidden = !file;
  if (file)
    el('file-info').textContent =
      `${file.name} · ${(file.size / 1024 / 1024).toLocaleString('vi-VN', { maximumFractionDigits: 1 })} MB`;
};
el('upload-clear').onclick = () => {
  el('file').value = '';
  el('file').onchange();
  clear();
  el('photo').hidden = true;
  el('photo').removeAttribute('src');
  el('placeholder').hidden = false;
};
el('focus-select').onclick = () => upload.toggle();
el('photo').onclick = (event) => upload.click(event);
for (const name of ['esp32', 'phone', 'upload']) el('src-' + name).onclick = () => pickSource(name);
for (const value of ['auto', 'manual']) el('mode-' + value).onclick = () => pickMode(value);
el('camera-start').onclick = () => camera.start();
el('camera-stop').onclick = () => camera.stop();
el('phone-freeze').onclick = () => camera.toggleFreeze();
el('phone-capture').onclick = () => camera.manual();
el('phone-facing').onchange = () => {
  camera.stop();
  clear();
};
for (const action of ['start', 'stop', 'capture', 'home', 'replay', 'greeting'])
  el('device-' + action).onclick = () => device.command(action);
el('device-id').onchange = () => {
  device.lastResult = null;
  device.lastFrame = null;
  device.poll();
};
el('resume-round').onclick = () =>
  el('camera-source').value === 'cloud' ? device.command('start') : camera.start();
el('speak').onclick = () => audio.play(current);
el('greeting').onclick = async () => {
  try {
    await audio.play(await api.json('/api/greeting'));
  } catch (e) {
    error(e.message);
  }
};
el('history-toggle').onclick = () => {
  const all = el('rows').classList.toggle('all');
  el('history-toggle').setAttribute('aria-expanded', String(all));
  el('history-toggle').textContent = all ? 'Thu gọn' : 'Xem tất cả';
};
el('stats-open').onclick = () => showStats(true);
el('stats-close').onclick = () => showStats(false);
el('settings-open').onclick = () => el('settings-dialog').showModal();
el('settings-close').onclick = () => el('settings-dialog').close();
el('connect').onclick = connect;
el('theme').onclick = () => {
  const light = document.documentElement.dataset.theme !== 'light';
  document.documentElement.dataset.theme = light ? 'light' : 'dark';
  try {
    localStorage.setItem('theme', light ? 'light' : 'dark');
  } catch {}
};
try {
  document.documentElement.dataset.theme = localStorage.getItem('theme') || 'dark';
} catch {}
window.addEventListener('resize', draw);
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    camera.stop();
    audio.stop();
  } else device.poll();
});
window.addEventListener('pagehide', () => {
  camera.stop();
  upload.cancel();
  streamAbort?.abort();
  audio.stop();
});
async function health() {
  try {
    const state = await (await fetch('/health')).json();
    localOnly = state.mode === 'local-only';
    el('key-section').hidden = localOnly;
    el('health').dataset.state = state.ready ? 'ok' : 'wait';
    el('health').textContent = state.ready
      ? localOnly
        ? 'Pi sẵn sàng · chạy local'
        : state.cloud_ready
          ? 'Pi + Supabase sẵn sàng'
          : 'Pi sẵn sàng · đang kết nối Cloud'
      : 'Đang khởi động';
  } catch {
    el('health').dataset.state = 'off';
    el('health').textContent = 'Không kết nối được Pi';
  }
}
setInterval(health, 15000);
syncSource();
health().then(() => {
  if (location.hash === '#bieu-do') showStats(true);
  if (localOnly) connect();
  else el('settings-dialog').showModal();
});
