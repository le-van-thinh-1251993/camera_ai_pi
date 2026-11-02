import test from 'node:test';
import assert from 'node:assert/strict';
import { AudioPlayer } from '../../static/js/audio.js';
test('A result is spoken automatically once; explicit replay is still allowed', async () => {
  let played = 0;
  globalThis.Audio = class {
    pause() {}
    async play() {
      played++;
    }
  };
  const player = new AudioPlayer(
    {
      async request() {
        return {
          async blob() {
            return new Blob(['wav']);
          },
        };
      },
    },
    (message) => assert.fail(message),
    () => {},
  );
  const event = { id: 'same-cloud-result', audio_url: '/api/jobs/id/audio.wav' };
  await player.play(event, true);
  await player.play(event, true);
  assert.equal(played, 1);
  await player.play(event, false);
  assert.equal(played, 2);
  player.stop();
});
