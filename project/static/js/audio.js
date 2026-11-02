export class AudioPlayer {
  constructor(api, onError, onPlayed) {
    this.api = api;
    this.onError = onError;
    this.onPlayed = onPlayed;
    this.audio = new Audio();
    this.version = 0;
    this.url = null;
    this.spoken = new Set();
  }
  stop() {
    this.version++;
    this.pending?.abort();
    this.audio.pause();
    this.audio.onended = null;
    this.audio.onerror = null;
    if (this.url) URL.revokeObjectURL(this.url);
    this.url = null;
  }
  async play(event, automatic = false) {
    if (!event?.audio_url) {
      this.onError('Chưa có âm thanh cho kết quả này.');
      return;
    }
    if (automatic && this.spoken.has(event.id)) return;
    if (automatic) {
      this.spoken.add(event.id);
      if (this.spoken.size > 200) this.spoken.delete(this.spoken.values().next().value);
    }
    this.stop();
    const version = this.version;
    this.pending = new AbortController();
    try {
      const blob = await (
        await this.api.request(event.audio_url, { signal: this.pending.signal })
      ).blob();
      if (version !== this.version) return;
      this.url = URL.createObjectURL(blob);
      this.audio.src = this.url;
      this.audio.loop = false;
      this.audio.onended = () => {
        if (version === this.version) this.onPlayed(event);
      };
      this.audio.onerror = () =>
        this.onError('Không phát được âm thanh. Kiểm tra âm lượng hoặc bấm Đọc lại.');
      await this.audio.play();
    } catch (error) {
      if (version !== this.version) return;
      this.onError(
        error.name === 'NotAllowedError'
          ? 'Bấm “Đọc lại” để cho phép trình duyệt phát tiếng.'
          : error.message,
      );
    }
  }
}
