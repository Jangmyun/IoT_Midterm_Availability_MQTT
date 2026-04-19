import test from 'node:test';
import assert from 'node:assert/strict';
import {
  buildPublishAttemptKey,
  isBrokerActivityStale,
  shouldSuppressDuplicatePublish,
} from './usePublisher.js';

test('buildPublishAttemptKey creates a stable signature from event inputs', () => {
  const key = buildPublishAttemptKey({
    type: 'MOTION',
    buildingId: 'nth-4',
    cameraId: 'cam-01',
    description: 'manual',
  });

  assert.equal(key, 'MOTION\u0001nth-4\u0001cam-01\u0001manual');
});

test('shouldSuppressDuplicatePublish blocks identical publishes within the guard window', () => {
  const key = buildPublishAttemptKey({
    type: 'INTRUSION',
    buildingId: 'nth-4',
    cameraId: 'cam-01',
    description: 'manual',
  });

  assert.equal(
    shouldSuppressDuplicatePublish({ key, at: 1_000 }, key, 1_050),
    true,
  );
});

test('shouldSuppressDuplicatePublish allows different events or later retries', () => {
  const previousKey = buildPublishAttemptKey({
    type: 'INTRUSION',
    buildingId: 'nth-4',
    cameraId: 'cam-01',
    description: 'manual',
  });
  const nextKey = buildPublishAttemptKey({
    type: 'MOTION',
    buildingId: 'nth-4',
    cameraId: 'cam-01',
    description: 'manual',
  });

  assert.equal(
    shouldSuppressDuplicatePublish({ key: previousKey, at: 1_000 }, nextKey, 1_050),
    false,
  );
  assert.equal(
    shouldSuppressDuplicatePublish({ key: previousKey, at: 1_000 }, previousKey, 1_500),
    false,
  );
});

test('isBrokerActivityStale returns true only after the keepalive grace window elapses', () => {
  assert.equal(isBrokerActivityStale(10_000, 17_000, 5), false);
  assert.equal(isBrokerActivityStale(10_000, 17_750, 5), false);
  assert.equal(isBrokerActivityStale(10_000, 17_751, 5), true);
});
