import { el, requestId } from './api.js?v=dash-a-5';
export class DeviceController {
  constructor(api, ui) {
    this.api = api;
    this.ui = ui;
    this.active = false;
    this.polling = false;
    this.lastResult = null;
    this.lastFrame = null;
    this.timer = null;
    this.liveOn = false;
    this.liveChecking = false;
    this.liveTimer = null;
    // Che do vua bam tren web, giu nut cho toi khi ESP32 xac nhan (khong nhay qua lai).
    this.pendingMode = null;
    this.fastUntil = 0;
  }
  path() {
    const id = el('device-id').value.trim();
    if (!/^[A-Za-z0-9_-]{1,32}$/.test(id)) throw new Error('Mã thiết bị không hợp lệ.');
    return '/api/cloud/device/' + encodeURIComponent(id);
  }
  activate(value) {
    this.active = value;
    clearTimeout(this.timer);
    if (value) {
      this.liveStart();
      this.poll();
    } else this.liveStop();
  }
  // Live MJPEG tu ESP32, chuyen tiep qua Pi (/api/esp32/live/stream).
  liveStart() {
    if (this.liveOn || this.liveChecking || !this.active) return;
    clearTimeout(this.liveTimer);
    this.liveCheck();
  }
  async liveCheck() {
    this.liveChecking = true;
    try {
      const status = await this.api.json('/api/esp32/live/status');
      if (this.active && status.online && status.camera) return this.liveShow();
    } catch {
      // ESP32 chua len mang: thu lai sau.
    } finally {
      this.liveChecking = false;
    }
    if (this.active && !this.liveOn) this.liveTimer = setTimeout(() => this.liveStart(), 5000);
  }
  liveShow() {
    const img = el('device-live');
    const key = this.api.key ? '&key=' + encodeURIComponent(this.api.key) : '';
    img.onerror = () => {
      this.liveStop();
      if (this.active) this.liveTimer = setTimeout(() => this.liveStart(), 3000);
    };
    img.src = '/api/esp32/live/stream?t=' + Date.now() + key;
    img.hidden = false;
    el('device-live-badge').hidden = false;
    el('placeholder').hidden = true;
    this.liveOn = true;
  }
  liveStop() {
    clearTimeout(this.liveTimer);
    const img = el('device-live');
    img.onerror = null;
    img.removeAttribute('src');
    img.hidden = true;
    el('device-live-badge').hidden = true;
    this.liveOn = false;
  }
  async command(action) {
    if (action === 'auto' || action === 'manual')
      this.pendingMode = { mode: action, until: Date.now() + 8000 };
    this.fastUntil = Date.now() + 8000; // hoi trang thai nhanh trong luc cho ESP32 lam theo
    try {
      el('device-error').textContent = '';
      const state = await this.api.post(this.path() + '/command', {
        action,
        request_id: 'web-' + requestId(),
        count_all: el('count-all').checked,
      });
      this.render(state);
      this.poll();
    } catch (error) {
      el('device-error').textContent = error.message;
    }
  }
  render(state) {
    this.state = state;
    const pending = this.pendingMode;
    if (pending && (state.mode === pending.mode || Date.now() > pending.until))
      this.pendingMode = null;
    const mode = this.pendingMode ? this.pendingMode.mode : state.mode;
    el('device-status').textContent =
      `${state.online ? '● Đã kết nối' : '○ Chưa nhận trạng thái'} · ${mode.toUpperCase()} · ${state.running ? 'Camera đang chạy' : state.view === 'result' ? 'Đã có kết quả' : 'Camera đang dừng'}${state.control_pending ? ' · Đang chờ nhận lệnh' : ''}`;
    for (const m of ['auto', 'manual'])
      el('mode-' + m).setAttribute('aria-pressed', String(mode === m));
    el('device-capture').disabled = !state.online || !state.can_capture; // chup ngay ca AUTO lan MANUAL
    el('device-replay').disabled = !state.result;
    el('hold-progress').value = state.hold_seconds;
    el('hold-status').textContent =
      state.mode === 'auto' && state.hold_seconds > 0
        ? `Tự chụp sau ${Math.max(0, 2 - state.hold_seconds)
            .toFixed(1)
            .replace('.', ',')} s`
        : '';
    el('resume-round').hidden = state.view !== 'result';
  }
  async poll() {
    if (!this.active || this.polling || document.hidden) return;
    clearTimeout(this.timer);
    this.polling = true;
    try {
      const state = await this.api.json(this.path() + '/state');
      if (!this.active) return;
      this.render(state);
      if (state.view === 'result') this.liveStop();
      else this.liveStart();
      if (state.result && state.view === 'result' && state.result.id !== this.lastResult) {
        this.lastResult = state.result.id;
        await this.ui.result({ ...state.result, preview_url: this.path() + '/preview.jpg' }, false);
      } else if (
        !this.liveOn &&
        state.frame_seq >= 0 &&
        state.frame_seq !== this.lastFrame &&
        state.view !== 'result'
      ) {
        this.lastFrame = state.frame_seq;
        await this.ui.image(this.path() + '/preview.jpg');
      }
    } catch (error) {
      el('device-error').textContent = error.message;
    } finally {
      this.polling = false;
      if (this.active)
        this.timer = setTimeout(
          () => this.poll(),
          Date.now() < this.fastUntil ? 400 : this.state?.running ? 1500 : 5000,
        );
    }
  }
}
