import { el } from './api.js?v=cloud-v6-camera-upload-8';

async function optimizeImage(file) {
  let bitmap;
  if (typeof createImageBitmap === 'function') {
    bitmap = await createImageBitmap(file);
  } else {
    bitmap = await new Promise((resolve, reject) => {
      const image = new Image();
      image.onload = () => resolve(image);
      image.onerror = () => reject(new Error('Không đọc được ảnh để tối ưu.'));
      image.src = URL.createObjectURL(file);
    });
  }
  const scale = Math.min(1, 1280 / Math.max(bitmap.width, bitmap.height));
  const canvas = document.createElement('canvas');
  canvas.width = Math.max(1, Math.round(bitmap.width * scale));
  canvas.height = Math.max(1, Math.round(bitmap.height * scale));
  canvas.getContext('2d').drawImage(bitmap, 0, 0, canvas.width, canvas.height);
  bitmap.close?.();
  const blob = await new Promise((resolve) => canvas.toBlob(resolve, 'image/jpeg', 0.8));
  if (!blob) throw new Error('Không thể tối ưu ảnh trước khi gửi.');
  return new File([blob], 'upload.jpg', { type: 'image/jpeg' });
}

export class UploadController {
  constructor(jobs, ui) {
    this.jobs = jobs;
    this.ui = ui;
    this.focus = null;
    this.picking = false;
    this.generation = 0;
    this.busy = false;
  }
  cancel() {
    this.generation++;
    this.abort?.abort();
    this.busy = false;
    this.ui.busy(false);
  }
  choose() {
    this.cancel();
    this.focus = null;
    this.picking = false;
    const file = el('file').files[0];
    el('focus-select').disabled = !file;
    el('focus-select').textContent = 'Chọn tay trong ảnh';
    if (this.url) URL.revokeObjectURL(this.url);
    if (!file) return;
    this.url = URL.createObjectURL(file);
    this.ui.preview(this.url);
    el('focus-status').textContent = 'Ảnh nhiều người: có thể chọn vùng tay cần đếm.';
  }
  toggle() {
    if (this.busy) return;
    if (this.focus || this.picking) {
      this.focus = null;
      this.picking = false;
      el('focus-select').textContent = 'Chọn tay trong ảnh';
      this.ui.redraw();
      return;
    }
    this.picking = true;
    el('focus-select').textContent = 'Hủy chọn vùng';
    el('focus-status').textContent = 'Chạm vào giữa lòng bàn tay trong ảnh.';
  }
  geometry() {
    const photo = el('photo'),
      r = photo.getBoundingClientRect();
    if (!photo.naturalWidth) return null;
    const scale = Math.min(r.width / photo.naturalWidth, r.height / photo.naturalHeight),
      w = photo.naturalWidth * scale,
      h = photo.naturalHeight * scale;
    return { r, w, h, x: (r.width - w) / 2, y: (r.height - h) / 2 };
  }
  click(event) {
    if (!this.picking || this.busy) return;
    const g = this.geometry();
    if (!g) return;
    const x = (event.clientX - g.r.left - g.x) / g.w,
      y = (event.clientY - g.r.top - g.y) / g.h;
    if (x < 0 || y < 0 || x > 1 || y > 1) return;
    this.focus = [x, y];
    this.picking = false;
    el('focus-select').textContent = 'Bỏ vùng đã chọn';
    el('focus-status').textContent = 'Nhận diện ảnh để đếm tay trong vùng vàng.';
    this.ui.redraw();
  }
  draw() {
    if (!this.focus) return;
    const g = this.geometry();
    if (!g) return;
    const canvas = el('marks'),
      ctx = canvas.getContext('2d');
    const x = Math.max(0, Math.min(0.52, this.focus[0] - 0.24)),
      y = Math.max(0, Math.min(0.52, this.focus[1] - 0.24));
    ctx.strokeStyle = '#ffcd62';
    ctx.lineWidth = 3;
    ctx.strokeRect(g.x + x * g.w, g.y + y * g.h, 0.48 * g.w, 0.48 * g.h);
  }
  async submit(event) {
    event.preventDefault();
    if (this.busy) return;
    const file = el('file').files[0];
    if (!file) {
      this.ui.error('Vui lòng chọn một ảnh JPEG/PNG.');
      return;
    }
    if (file.size > 15 * 1024 * 1024) {
      this.ui.error('Ảnh vượt 15 MB. Hãy xuất JPEG dung lượng nhỏ hơn.');
      return;
    }
    this.abort = new AbortController();
    const generation = this.generation;
    this.busy = true;
    this.ui.busy(true);
    try {
      this.ui.status?.('Đang nén ảnh để gửi nhanh hơn…');
      const optimized = await optimizeImage(file);
      const result = await this.jobs.submit(
        [optimized],
        { source: 'upload', mode: 'upload', focus: this.focus, countAll: el('count-all').checked },
        this.abort.signal,
      );
      if (generation === this.generation) await this.ui.result(result);
    } catch (error) {
      if (!this.abort.signal.aborted) this.ui.error(error.message);
    } finally {
      if (generation === this.generation) {
        this.busy = false;
        this.ui.busy(false);
      }
    }
  }
}
