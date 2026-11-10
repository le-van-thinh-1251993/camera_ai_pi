import test from 'node:test';
import assert from 'node:assert/strict';
import { CameraController } from '../../static/js/camera.js';
import { CloudJobs } from '../../static/js/cloud.js';

const elements = new Map();
globalThis.document = {
  getElementById: (id) => {
    if (!elements.has(id))
      elements.set(id, {
        value: 'auto',
        checked: false,
        hidden: false,
        disabled: false,
        srcObject: null,
      });
    return elements.get(id);
  },
  createElement: (tag) => ({ tagName: tag.toUpperCase(), srcObject: null }),
};
elements.set('phone-mode', { value: 'manual' });
globalThis.cancelAnimationFrame = () => {};

test('Auto waits two seconds locally, sends ONE image and stops even with a slow Cloud', async () => {
  let calls = 0,
    results = 0,
    stopped = 0;
  const jobs = {
    async submit(files, options) {
      calls++;
      assert.equal(files.length, 1);
      assert.equal(options.mode, 'manual');
      assert.equal(options.captureMode, 'auto');
      assert.deepEqual(options.times, [0]);
      await new Promise((resolve) => setTimeout(resolve, 3000));
      return { id: 'result', cloud: true, confirmed: true, finger_count: 2 };
    },
  };
  const camera = new CameraController(jobs, {
    status() {},
    progress() {},
    preview() {
      assert.equal(camera.running, true);
    },
    error(message) {
      assert.fail(message);
    },
    async result() {
      results++;
    },
  });
  camera.media = {
    getTracks: () => [
      {
        stop() {
          stopped++;
        },
      },
    ],
  };
  camera.running = true;
  camera.capture = async () => {
    camera.capturedAt = performance.now();
    return new Blob(['sample'], { type: 'image/jpeg' });
  };
  const started = performance.now();
  await camera.autoLoop(0, new AbortController().signal);
  assert(performance.now() - started >= 4900);
  assert.equal(calls, 1);
  assert.equal(results, 1);
  assert.equal(stopped, 1);
  assert.equal(camera.running, false);
});

test('Camera stops and shows the captured frame before a slow Cloud submit', async () => {
  let submittedWhileRunning = null;
  const camera = new CameraController(
    {
      async submit() {
        submittedWhileRunning = camera.running;
        await new Promise((resolve) => setTimeout(resolve, 20));
        return { id: 'result', cloud: true, confirmed: true };
      },
    },
    {
      status() {},
      progress() {},
      preview() {
        assert.equal(camera.running, true);
      },
      error(message) {
        assert.fail(message);
      },
      async result() {},
    },
  );
  camera.media = { getTracks: () => [{ stop() {} }] };
  camera.running = true;
  camera.capture = async () => new Blob(['sample'], { type: 'image/jpeg' });
  await camera.manual();
  assert.equal(submittedWhileRunning, false);
});

test('Upload and camera submit only to the Cloud queue and wait for a committed result', async () => {
  const calls = [];
  const api = {
    async json(url, options) {
      calls.push(url);
      if (options?.method === 'POST')
        return { id: 'one', cloud: true, status_url: '/api/jobs/one' };
      return { status: 'completed', result: { id: 'one', cloud: true, confirmed: true } };
    },
  };
  const jobs = new CloudJobs(api, () => {});
  const result = await jobs.submit(
    [new Blob(['jpeg'])],
    { source: 'upload', mode: 'upload' },
    new AbortController().signal,
  );
  assert(result.cloud);
  assert.deepEqual(calls, ['/api/upload', '/api/jobs/one']);
});

test('A dropped poll reconnects without uploading another image or resetting the hold', async () => {
  let uploads = 0,
    polls = 0;
  const api = {
    async json(url, options) {
      if (options?.method === 'POST') {
        uploads++;
        return { cloud: true, status_url: '/api/jobs/once' };
      }
      if (++polls === 1) throw new TypeError('Failed to fetch');
      return { status: 'completed', result: { id: 'once', cloud: true, confirmed: true } };
    },
  };
  const result = await new CloudJobs(api, () => {}).submit(
    [new Blob(['sample'])],
    { source: 'browser', mode: 'manual' },
    new AbortController().signal,
  );
  assert.equal(result.id, 'once');
  assert.equal(uploads, 1);
  assert.equal(polls, 2);
});
