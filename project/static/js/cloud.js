import { requestId, sleep } from './api.js?v=cloud-v6-camera-upload-4';
export class CloudJobs {
  constructor(api, status) {
    this.api = api;
    this.status = status;
  }
  async submit(files, options, signal) {
    const id = requestId(),
      data = new FormData();
    for (let i = 0; i < files.length; i++)
      data.append(options.source === 'upload' ? 'file' : 'files', files[i], `${i}.jpg`);
    for (const [key, value] of Object.entries({
      request_id: id,
      mode: options.mode,
      capture_mode: options.captureMode || options.mode,
      captures_ms: JSON.stringify(options.times || [0]),
      count_all: String(Boolean(options.countAll)),
      focus: JSON.stringify(options.focus || null),
      retry_index: String(options.attempt || 0),
    }))
      data.append(key, value);
    this.status('Đang gửi ảnh lên Supabase…');
    let job;
    for (let attempt = 0; attempt < 3; attempt++) {
      try {
        job = await this.api.json(options.source === 'upload' ? '/api/upload' : '/api/camera', {
          method: 'POST',
          body: data,
          signal,
        });
        break;
      } catch (error) {
        if (
          signal.aborted ||
          attempt === 2 ||
          (error.status && ![502, 503, 504].includes(error.status))
        )
          throw error;
        this.status('Kết nối Cloud gián đoạn. Đang thử lại cùng mã yêu cầu…');
        await sleep((attempt + 1) * 1000, signal);
      }
    }
    if (!job.cloud && !job.fallback)
      throw new Error('Máy chủ chưa dùng luồng Supabase mới.');
    const deadline = performance.now() + 120000;
    let interval = 250;
    let pollFailures = 0;
    while (performance.now() < deadline) {
      let state;
      try {
        state = await this.api.json(job.status_url, { signal });
        pollFailures = 0;
      } catch (error) {
        if (
          signal.aborted ||
          ++pollFailures > 5 ||
          (error.status && ![502, 503, 504].includes(error.status))
        )
          throw error;
        this.status('Đang nối lại để lấy kết quả Cloud. Ảnh không bị gửi lại.');
        await sleep(1000, signal);
        continue;
      }
      if (state.status === 'completed' || state.status === 'acked') {
        if (!state.result?.cloud && !state.result?.fallback)
          throw new Error('Kết quả chưa được xác nhận qua Cloud hoặc fallback cục bộ.');
        return state.result;
      }
      if (state.status === 'failed')
        throw new Error(`Cloud không xử lý được ảnh (${state.error_code || 'ảnh chưa hợp lệ'}).`);
      this.status(
        state.status === 'processing'
          ? 'Pi đang nhận diện ảnh từ Supabase…'
          : 'Ảnh đã lên Supabase. Đang chờ Pi nhận việc…',
      );
      await sleep(interval, signal);
      interval = Math.min(1000, interval * 1.2);
    }
    throw new Error('Chờ Pi quá lâu. Kiểm tra kết nối Supabase và /api/diagnostics.');
  }
}
