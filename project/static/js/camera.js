import { el, sleep } from './api.js';
export class CameraController {
  constructor(jobs, ui) {
    this.jobs = jobs;
    this.ui = ui;
    this.generation = 0;
    this.running = false;
    this.frozen = false;
    this.media = null;
    this.busy = false;
  }
  showPreview(url) {
    if (typeof this.ui.preview === 'function') this.ui.preview(url);
  }
  stop(keep = false, cancel = true) {
    if (cancel) {
      this.generation++;
      this.abort?.abort();
    }
    cancelAnimationFrame(this.animation);
    for (const track of this.media?.getTracks() || []) track.stop();
    this.media = null;
    this.running = false;
    this.busy = false;
    el('camera-video').srcObject = null;
    el('live-preview').hidden = true;
    el('camera-start').disabled = false;
    el('camera-stop').disabled = true;
    el('phone-freeze').disabled = true;
    el('phone-capture').disabled = true;
    if (!keep) this.ui.status('Camera đang tắt.');
  }
  async start() {
    this.stop();
    this.ui.start();
    const generation = this.generation;
    this.abort = new AbortController();
    this.frozen = false;
    try {
      if (!window.isSecureContext || !navigator.mediaDevices?.getUserMedia)
        throw new Error(
          'Camera điện thoại cần HTTPS. Hãy mở link ngrok HTTPS và cấp quyền camera.',
        );
      const media = await navigator.mediaDevices.getUserMedia({
        audio: false,
        video: {
          width: { ideal: 1280 },
          height: { ideal: 720 },
          facingMode: { ideal: el('phone-facing').value },
        },
      });
      if (generation !== this.generation) {
        media.getTracks().forEach((track) => track.stop());
        return;
      }
      this.media = media;
      const video = el('camera-video');
      video.muted = true;
      video.playsInline = true;
      video.setAttribute('autoplay', '');
      video.setAttribute('muted', '');
      video.setAttribute('playsinline', '');
      video.setAttribute('webkit-playsinline', '');
      video.setAttribute('x5-playsinline', '');
      video.disablePictureInPicture = true;
      video.controls = false;
      video.srcObject = media;
      await video.play();
      if (generation !== this.generation) return;
      this.running = true;
      el('live-preview').hidden = false;
      el('photo').hidden = true;
      el('placeholder').hidden = true;
      el('camera-start').disabled = true;
      el('camera-stop').disabled = false;
      el('phone-freeze').disabled = false;
      el('phone-capture').disabled = el('phone-mode').value !== 'manual';
      const paint = () => {
        if (generation !== this.generation) return;
        const canvas = el('live-preview');
        if (!this.frozen && video.videoWidth) {
          if (canvas.width !== video.videoWidth || canvas.height !== video.videoHeight) {
            canvas.width = video.videoWidth;
            canvas.height = video.videoHeight;
          }
          canvas.getContext('2d').drawImage(video, 0, 0, canvas.width, canvas.height);
        }
        this.animation = requestAnimationFrame(paint);
      };
      paint();
      for (const track of media.getVideoTracks())
        track.addEventListener('ended', () => {
          if (generation === this.generation) {
            this.stop();
            this.ui.error('Camera đã ngắt kết nối.');
          }
        });
      if (el('phone-mode').value === 'auto') this.autoLoop(generation, this.abort.signal);
      else this.ui.status('Manual: căn tay, dừng hình nếu cần, rồi bấm Nhận diện.');
    } catch (error) {
      if (generation === this.generation) {
        this.stop();
        this.ui.error(
          error.name === 'NotAllowedError'
            ? 'Bạn chưa cấp quyền camera cho trình duyệt.'
            : error.message,
        );
      }
    }
  }
  toggleFreeze() {
    if (!this.running || this.busy) return;
    this.frozen = !this.frozen;
    this.ui.status(
      this.frozen ? 'Đã dừng hình. Bấm Nhận diện hoặc Tiếp tục hình.' : 'Camera đang chạy.',
    );
  }
  async capture(signal) {
    const video = el('camera-video');
    while (!video.videoWidth || video.readyState < 2) await sleep(50, signal);
    const canvas = document.createElement('canvas'),
      scale = Math.min(1, 1280 / Math.max(video.videoWidth, video.videoHeight));
    canvas.width = Math.round(video.videoWidth * scale);
    canvas.height = Math.round(video.videoHeight * scale);
    this.capturedAt = performance.now();
    canvas
      .getContext('2d')
      .drawImage(this.frozen ? el('live-preview') : video, 0, 0, canvas.width, canvas.height);
    const blob = await new Promise((resolve) => canvas.toBlob(resolve, 'image/jpeg', 0.85));
    if (!blob) throw new Error('Không chụp được ảnh camera.');
    return blob;
  }
  async burst(signal) {
    const start = performance.now();
    while (performance.now() - start < 2000) {
      this.ui.progress(Math.min(2, (performance.now() - start) / 1000));
      await sleep(50, signal);
    }
    this.ui.progress(2);
    return { files: [await this.capture(signal)], times: [0] };
  }
  async autoLoop(generation, signal) {
    try {
      this.ui.status('Căn tay trong 2 giây. Chỉ gửi một ảnh để nhận diện.');
      const batch = await this.burst(signal);
      if (generation !== this.generation) return;
      this.busy = true;
      this.showPreview(URL.createObjectURL(batch.files[0]));
      this.stop(true, false);
      const event = await this.jobs.submit(
        batch.files,
        {
          source: 'browser',
          mode: 'manual',
          captureMode: 'auto',
          times: [0],
          countAll: el('count-all').checked,
        },
        signal,
      );
      if (generation !== this.generation) return;
      await this.ui.result(event);
    } catch (error) {
      if (!signal.aborted && generation === this.generation) {
        this.stop();
        this.ui.error(error.message);
      }
    }
  }
  async manual() {
    if (!this.running || this.busy || el('phone-mode').value !== 'manual') return;
    const generation = this.generation,
      signal = this.abort?.signal || new AbortController().signal;
    this.busy = true;
    this.frozen = true;
    try {
      const blob = await this.capture(signal);
      this.showPreview(URL.createObjectURL(blob));
      this.stop(true, false);
      const event = await this.jobs.submit(
        [blob],
        { source: 'browser', mode: 'manual', countAll: el('count-all').checked },
        signal,
      );
      if (generation !== this.generation) return;
      await this.ui.result(event);
    } catch (error) {
      if (!signal.aborted && generation === this.generation) {
        this.busy = false;
        this.ui.error(error.message);
      }
    }
  }
}
