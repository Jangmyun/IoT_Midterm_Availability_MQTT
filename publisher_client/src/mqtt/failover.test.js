import test from 'node:test';
import assert from 'node:assert/strict';
import { getNextBrokerIndex, parseBrokerCandidates } from './failover.js';

test('parseBrokerCandidates supports comma and newline separated websocket URLs', () => {
  assert.deepEqual(
    parseBrokerCandidates('ws://192.168.0.9:9001,\nws://192.168.0.16:9001 ws://192.168.0.7:9001'),
    [
      'ws://192.168.0.9:9001/',
      'ws://192.168.0.16:9001/',
      'ws://192.168.0.7:9001/',
    ],
  );
});

test('parseBrokerCandidates drops invalid and duplicate URLs', () => {
  assert.deepEqual(
    parseBrokerCandidates('ws://192.168.0.9:9001 mqtt://broker:1883 ws://192.168.0.9:9001'),
    ['ws://192.168.0.9:9001/'],
  );
});

test('getNextBrokerIndex rotates through candidates', () => {
  assert.equal(getNextBrokerIndex(-1, 3), 0);
  assert.equal(getNextBrokerIndex(0, 3), 1);
  assert.equal(getNextBrokerIndex(2, 3), 0);
  assert.equal(getNextBrokerIndex(0, 0), -1);
});
