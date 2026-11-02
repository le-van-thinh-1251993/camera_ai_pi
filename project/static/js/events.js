import { sleep } from './api.js?v=cloud-v6-camera-upload-3';
export async function watchEvents(api, onEvent, signal) {
  while (!signal.aborted) {
    try {
      const response = await api.request('/api/events', { signal, stream: true }),
        reader = response.body.getReader(),
        decoder = new TextDecoder();
      const cancel = () => reader.cancel().catch(() => {});
      signal.addEventListener('abort', cancel, { once: true });
      let buffer = '';
      try {
        while (!signal.aborted) {
          const { value, done } = await reader.read();
          if (done) break;
          buffer += decoder.decode(value, { stream: true });
          let cut;
          while ((cut = buffer.indexOf('\n\n')) >= 0) {
            const part = buffer.slice(0, cut);
            buffer = buffer.slice(cut + 2);
            if (part.startsWith('data: ')) onEvent(JSON.parse(part.slice(6)));
          }
        }
      } finally {
        signal.removeEventListener('abort', cancel);
        await reader.cancel().catch(() => {});
        reader.releaseLock();
      }
    } catch (error) {
      if (signal.aborted || error.status === 401) return;
    }
    await sleep(3000, signal).catch(() => {});
  }
}
