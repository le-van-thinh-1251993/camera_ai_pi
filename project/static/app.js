import { Api, el } from './js/api.js?v=cloud-v6-camera-upload-3';
import { CloudJobs } from './js/cloud.js?v=cloud-v6-camera-upload-3';
import { AudioPlayer } from './js/audio.js?v=cloud-v6-camera-upload-3';
import { CameraController } from './js/camera.js?v=cloud-v6-camera-upload-3';
import { UploadController } from './js/upload.js?v=cloud-v6-camera-upload-3';
import { DeviceController } from './js/device.js?v=cloud-v6-camera-upload-3';
import { watchEvents } from './js/events.js?v=cloud-v6-camera-upload-3';

const api = new Api();
let current = null,
  mode = 'upload',
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
    ? `Giơ ${event.finger_count} ngón tay`
    : event.partial && event.visible_finger_count
      ? `Thấy rõ ít nhất ${event.visible_finger_count} ngón (bàn tay bị che)`
    : event.gesture === 'No_hand'
      ? 'Không thấy bàn tay'
      : 'Chưa xác định cử chỉ';
function row(event) {
  if ((!event.confirmed && event.pipeline_version === 6) || seen.has(event.id)) return;
  seen.add(event.id);
  const tr = document.createElement('tr');
  tr.dataset.id = event.id;
  for (const value of [
    new Date(event.time).toLocaleString('vi-VN'),
    event.source === 'upload'
      ? 'Ảnh · Cloud'
      : event.source === 'browser'
        ? 'Camera · Cloud'
        : 'ESP32 · Cloud',
    gestureName(event),
    event.text || 'Chưa nhận diện được cử chỉ',
    'Đếm landmark',
  ]) {
    const td = document.createElement('td');
    td.textContent = value;
    tr.append(td);
  }
  el('rows').prepend(tr);
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
function clear() {
  el('hold-progress').value = 0;
  el('hold-status').textContent = 'Auto: giữ 2 giây. Manual: bấm nhận diện.';
  el('camera-actions').hidden = false;
  el('phone-manual-controls').hidden =
    el('phone-mode').value !== 'manual' || el('camera-source').value === 'cloud';
  current = null;
  renderVersion++;
  audio.stop();
  error('');
  el('gesture').textContent =
    'Hãy đưa cử chỉ vào đúng khung hình camera hoặc gửi ảnh để AI nhận diện';
  el('phrase').textContent = 'Chưa nhận diện được cử chỉ';
  el('speak').disabled = true;
  el('resume-round').hidden = true;
  for (const id of ['hand-summary', 'duration', 'speech-status', 'confidence'])
    el(id).textContent = '';
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
    ctx.fillStyle = '#68dfba';
    for (const hand of current.hands || [])
      for (const point of hand.landmarks || []) {
        ctx.beginPath();
        ctx.arc(ox + point.x * w, oy + point.y * h, 3, 0, 2 * Math.PI);
        ctx.fill();
      }
  }
  if (mode === 'upload') upload.draw();
}
async function image(url) {
  const version = renderVersion;
  const blob = await (await api.request(url)).blob();
  if (version !== renderVersion) return;
  const next = URL.createObjectURL(blob);
  if (photoURL) URL.revokeObjectURL(photoURL);
  photoURL = next;
  el('photo').onload = draw;
  el('photo').src = next;
  el('photo').hidden = false;
  el('placeholder').hidden = true;
  el('live-preview').hidden = true;
}
async function result(event, speak = true) {
  if (!event.confirmed) return;
  error('');
  const version = ++renderVersion;
  current = event;
  row(event);
  el('processing').hidden = true;
  el('gesture').textContent = gestureName(event);
  el('phrase').textContent = event.text;
  el('speak').disabled = !event.audio_url;
  el('hand-summary').textContent = (event.hands || [])
    .map((h, i) => `Tay ${i + 1}: ${h.finger_count ?? 'chưa rõ'} ngón`)
    .join(' · ');
  el('confidence').textContent = 'Cách nhận diện: điểm bàn tay MediaPipe';
  el('duration').textContent =
    `Xử lý AI: ${event.latency_ms || 0} ms · ${event.frames_checked || 1} ảnh · Supabase`;
  el('speech-status').textContent = event.audio_url
    ? 'Giọng đọc từ clip đã thu.'
    : 'Chưa có clip âm thanh.';
  el('resume-round').hidden = mode !== 'camera';
  if (mode === 'camera') {
    el('camera-options').open = false;
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
  start: () => {
    clear();
    el('camera-options').open = false;
  },
  status: (message) => {
    el('camera-status').textContent = message;
  },
  error,
  progress: (seconds) => {
    el('hold-progress').value = seconds;
    el('hold-status').textContent = `Giữ ổn định: ${seconds.toFixed(1)} / 2 giây`;
  },
  preview: (url) => {
    clear();
    if (photoURL) URL.revokeObjectURL(photoURL);
    photoURL = url;
    el('photo').src = url;
    el('photo').hidden = false;
    el('placeholder').hidden = true;
    el('live-preview').hidden = true;
    el('photo').onload = draw;
  },
  result,
});
function busy(value) {
  el('upload-button').disabled = value;
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
    el('photo').src = url;
    el('photo').hidden = false;
    el('placeholder').hidden = true;
    el('photo').onload = draw;
  },
});
const device = new DeviceController(api, { result, image });
function source() {
  camera.stop();
  const hardware = el('camera-source').value === 'cloud';
  el('camera-options').open = true;
  el('phone-manual-controls').hidden = hardware || el('phone-mode').value !== 'manual';
  el('phone-controls').hidden = hardware;
  el('device-controls').hidden = !hardware;
  el('camera-start').hidden = hardware;
  el('camera-stop').hidden = hardware;
  device.activate(mode === 'camera' && hardware);
}
function setMode(next) {
  if (mode === next) return;
  el('camera-options').open = true;
  upload.cancel();
  camera.stop();
  device.activate(false);
  clear();
  mode = next;
  for (const name of ['upload', 'camera']) {
    el('panel-' + name).hidden = name !== mode;
    el('tab-' + name).setAttribute('aria-selected', String(name === mode));
    el('tab-' + name).tabIndex = name === mode ? 0 : -1;
  }
  el('preview-title').textContent =
    mode === 'camera' ? 'Camera trực tiếp · Supabase' : 'Ảnh nhận diện · Supabase';
  el('photo').hidden = true;
  el('placeholder').hidden = false;
  if (mode === 'camera') source();
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
    el('connection').textContent = cloudReady
      ? '● Đã kết nối Pi + Supabase'
      : '◐ Đã xác thực Pi · đang kết nối Supabase…';
    el('settings-status').textContent = cloudReady
      ? 'Đã xác thực dashboard và Supabase.'
      : 'Đã xác thực dashboard. Supabase đang khởi tạo, thử gửi lại sau ít giây.';
    el('settings-dialog').close();
    error('');
    streamAbort = new AbortController();
    watchEvents(
      api,
      (event) => {
        row(event);
        if (event.device_id) device.poll();
      },
      streamAbort.signal,
    );
    source();
  } catch (e) {
    el('settings-status').textContent = e.message;
    error(e.message);
  }
}
el('form').onsubmit = (event) => upload.submit(event);
el('file').onchange = () => upload.choose();
el('focus-select').onclick = () => upload.toggle();
el('photo').onclick = (event) => upload.click(event);
el('tab-upload').onclick = () => setMode('upload');
el('tab-camera').onclick = () => setMode('camera');
for (const [index, name] of ['upload', 'camera'].entries())
  el('tab-' + name).onkeydown = (event) => {
    if (event.key === 'ArrowRight' || event.key === 'ArrowLeft') {
      setMode(index === 0 ? 'camera' : 'upload');
      el('tab-' + mode).focus();
    }
  };
el('camera-source').onchange = () => {
  clear();
  source();
};
el('camera-start').onclick = () => camera.start();
el('camera-stop').onclick = () => camera.stop();
el('phone-freeze').onclick = () => camera.toggleFreeze();
el('phone-capture').onclick = () => camera.manual();
for (const name of ['phone-mode', 'phone-facing'])
  el(name).onchange = () => {
    camera.stop();
    clear();
    el('camera-options').open = true;
  };
for (const action of ['auto', 'manual', 'start', 'stop', 'capture', 'home', 'replay', 'greeting'])
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
el('settings-open').onclick = () => el('settings-dialog').showModal();
el('settings-close').onclick = () => el('settings-dialog').close();
el('connect').onclick = connect;
el('preview-size').onclick = () => {
  el('preview-panel').classList.toggle('large-preview');
  el('preview-size').setAttribute(
    'aria-pressed',
    String(el('preview-panel').classList.contains('large-preview')),
  );
};
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
    el('health').textContent = state.ready
      ? state.cloud_ready
        ? '● Pi + Supabase sẵn sàng'
        : '● Model sẵn sàng · đang kết nối Cloud'
      : '● Đang khởi động';
  } catch {
    el('health').textContent = '○ Không kết nối được Pi';
  }
}
health();
setInterval(health, 15000);
el('settings-dialog').showModal();
