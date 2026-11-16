import test from 'node:test';
import assert from 'node:assert/strict';
import * as api from '../../static/js/api.js';

test('HOLD_SECONDS defaults to 5 and follows /health hold_seconds', () => {
  assert.equal(api.HOLD_SECONDS, 5);
  api.setHoldSeconds(3);
  assert.equal(api.HOLD_SECONDS, 3);
  for (const bad of [undefined, null, 'abc', 0, -2, 99]) {
    api.setHoldSeconds(bad);
    assert.equal(api.HOLD_SECONDS, 3);
  }
  api.setHoldSeconds('5');
  assert.equal(api.HOLD_SECONDS, 5);
});
