export const el = (id) => document.getElementById(id);
export function sleep(ms, signal) {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) return reject(new DOMException('Aborted', 'AbortError'));
    const done = () => {
      clearTimeout(timer);
      signal?.removeEventListener('abort', cancel);
      resolve();
    };
    const cancel = () => {
      clearTimeout(timer);
      reject(new DOMException('Aborted', 'AbortError'));
    };
    const timer = setTimeout(done, ms);
    signal?.addEventListener('abort', cancel, { once: true });
  });
}
export class Api {
  key = '';
  async request(url, options = {}) {
    const controller = new AbortController();
    const abort = () => controller.abort();
    if (options.signal?.aborted) abort();
    options.signal?.addEventListener('abort', abort, { once: true });
    const timeout = options.stream ? null : setTimeout(abort, 25000);
    try {
      const response = await fetch(url, {
        ...options,
        signal: controller.signal,
        headers: { 'X-API-Key': this.key, ...options.headers },
      });
      if (!response.ok) {
        const data = await response.json().catch(() => ({}));
        const error = new Error(
          typeof data.detail === 'string' ? data.detail : `Yêu cầu thất bại (${response.status})`,
        );
        error.status = response.status;
        throw error;
      }
      return response;
    } finally {
      clearTimeout(timeout);
      options.signal?.removeEventListener('abort', abort);
    }
  }
  async json(url, options = {}) {
    return (await this.request(url, options)).json();
  }
  async post(url, data, signal) {
    return this.json(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data),
      signal,
    });
  }
}
