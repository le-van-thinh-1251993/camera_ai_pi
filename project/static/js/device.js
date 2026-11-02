import { el, requestId } from './api.js?v=cloud-v6-camera-upload-3';
export class DeviceController {
  constructor(api, ui) {
    this.api = api;
    this.ui = ui;
    this.active = false;
    this.polling = false;
    this.lastResult = null;
    this.lastFrame = null;
    this.timer = null;
  }
  path() {
    const id = el('device-id').value.trim();
    if (!/^[A-Za-z0-9_-]{1,32}$/.test(id)) throw new Error('Mã thiết bị không hợp lệ.');
    return '/api/cloud/device/' + encodeURIComponent(id);
  }
  activate(value) {
    this.active = value;
    clearTimeout(this.timer);
    if (value) this.poll();
  }
  async command(action) {
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
    el('device-status').textContent =
      `${state.online ? '● Đã kết nối' : '○ Chưa nhận trạng thái'} · ${state.mode.toUpperCase()} · ${state.running ? 'Camera đang chạy' : state.view === 'result' ? 'Đã có kết quả' : 'Camera đang dừng'}${state.control_pending ? ' · Đang chờ nhận lệnh' : ''}`;
    for (const mode of ['auto', 'manual'])
      el('device-' + mode).setAttribute('aria-pressed', String(state.mode === mode));
    el('device-capture').disabled = state.mode !== 'manual' || !state.can_capture;
    el('device-replay').disabled = !state.result;
    el('hold-progress').value = state.hold_seconds;
    el('hold-status').textContent =
      state.mode === 'auto'
        ? `Giữ cử chỉ: ${state.hold_seconds.toFixed(1)} / 2 giây`
        : 'Manual: dừng hình rồi bấm nhận diện.';
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
      if (state.result && state.view === 'result' && state.result.id !== this.lastResult) {
        this.lastResult = state.result.id;
        await this.ui.result({ ...state.result, preview_url: this.path() + '/preview.jpg' }, false);
      } else if (
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
        this.timer = setTimeout(() => this.poll(), this.state?.running ? 1500 : 5000);
    }
  }
}
