import test from 'node:test';
import assert from 'node:assert/strict';
import { MSG_TYPES, buildTopic, buildMessage, inferPriority, nowUtc } from './message.js';

const PUB_ID = 'aaaaaaaa-0000-4000-8000-000000000001';
const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const TS_RE   = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/;

// ── nowUtc ────────────────────────────────────────────────────────────────────

test('nowUtc — ISO 8601 UTC 형식 (밀리초 없음)', () => {
  const ts = nowUtc();
  assert.match(ts, TS_RE);
  assert.equal(ts.endsWith('Z'), true);
  assert.equal(ts.includes('.'), false);
});

// ── inferPriority ─────────────────────────────────────────────────────────────

test('inferPriority — INTRUSION/DOOR_FORCED → HIGH, MOTION → MEDIUM', () => {
  assert.equal(inferPriority('INTRUSION'),   'HIGH');
  assert.equal(inferPriority('DOOR_FORCED'), 'HIGH');
  assert.equal(inferPriority('MOTION'),      'MEDIUM');
  assert.equal(inferPriority('UNKNOWN'),     null);
});

// ── buildTopic ────────────────────────────────────────────────────────────────

test('buildTopic — 정상 topic 생성', () => {
  assert.equal(buildTopic('MOTION',     'building-a', 'cam-01'), 'campus/data/motion/building-a/cam-01');
  assert.equal(buildTopic('DOOR_FORCED','building-a', 'cam-01'), 'campus/data/door/building-a/cam-01');
  assert.equal(buildTopic('INTRUSION',  'bldg-b',     'cam-99'), 'campus/data/intrusion/bldg-b/cam-99');
});

test('buildTopic — 빈 인자 → null', () => {
  assert.equal(buildTopic('MOTION', '',           'cam-01'), null);
  assert.equal(buildTopic('MOTION', 'building-a', ''),       null);
  assert.equal(buildTopic('',       'building-a', 'cam-01'), null);
  assert.equal(buildTopic('UNKNOWN','building-a', 'cam-01'), null);
});

// ── buildMessage ──────────────────────────────────────────────────────────────

test('buildMessage — MOTION: 필드 전체 확인', () => {
  const msg = buildMessage({
    publisherId: PUB_ID,
    type: 'MOTION',
    buildingId: 'building-a',
    cameraId: 'cam-01',
    description: 'test',
    qos: 1,
  });

  assert.match(msg.msg_id, UUID_RE);
  assert.equal(msg.type,             'MOTION');
  assert.equal(msg.priority,         'MEDIUM');
  assert.match(msg.timestamp,        TS_RE);
  assert.equal(msg.source.role,      'NODE');
  assert.equal(msg.source.id,        PUB_ID);
  assert.equal(msg.target.role,      'CORE');
  assert.equal(msg.target.id,        '');
  assert.equal(msg.route.original_node, PUB_ID);
  assert.equal(msg.route.hop_count,  0);
  assert.equal(msg.route.ttl,        8);
  assert.equal(msg.delivery.qos,     1);
  assert.equal(msg.delivery.dup,     false);
  assert.equal(msg.delivery.retain,  false);
  assert.equal(msg.payload.building_id, 'building-a');
  assert.equal(msg.payload.camera_id,   'cam-01');
  assert.equal(msg.payload.description, 'test');
});

test('buildMessage — INTRUSION: priority HIGH', () => {
  const msg = buildMessage({ publisherId: PUB_ID, type: 'INTRUSION', buildingId: 'b', cameraId: 'c' });
  assert.equal(msg.type,     'INTRUSION');
  assert.equal(msg.priority, 'HIGH');
});

test('buildMessage — DOOR_FORCED: priority HIGH', () => {
  const msg = buildMessage({ publisherId: PUB_ID, type: 'DOOR_FORCED', buildingId: 'b', cameraId: 'c' });
  assert.equal(msg.type,     'DOOR_FORCED');
  assert.equal(msg.priority, 'HIGH');
});

test('buildMessage — description 기본값 빈 문자열', () => {
  const msg = buildMessage({ publisherId: PUB_ID, type: 'MOTION', buildingId: 'b', cameraId: 'c' });
  assert.equal(msg.payload.description, '');
});

test('buildMessage — publisherId 없으면 throw', () => {
  assert.throws(
    () => buildMessage({ publisherId: '', type: 'MOTION', buildingId: 'b', cameraId: 'c' }),
    /publisherId required/,
  );
});

test('buildMessage — 알 수 없는 type이면 throw', () => {
  assert.throws(
    () => buildMessage({ publisherId: PUB_ID, type: 'INVALID', buildingId: 'b', cameraId: 'c' }),
    /unknown type/,
  );
});

test('buildMessage — msg_id는 호출마다 다른 UUID', () => {
  const a = buildMessage({ publisherId: PUB_ID, type: 'MOTION', buildingId: 'b', cameraId: 'c' });
  const b = buildMessage({ publisherId: PUB_ID, type: 'MOTION', buildingId: 'b', cameraId: 'c' });
  assert.notEqual(a.msg_id, b.msg_id);
});

test('buildMessage — JSON 직렬화/역직렬화 round-trip', () => {
  const orig = buildMessage({ publisherId: PUB_ID, type: 'INTRUSION', buildingId: 'bldg', cameraId: 'cam' });
  const parsed = JSON.parse(JSON.stringify(orig));
  assert.equal(parsed.msg_id,           orig.msg_id);
  assert.equal(parsed.type,             'INTRUSION');
  assert.equal(parsed.priority,         'HIGH');
  assert.equal(parsed.payload.building_id, 'bldg');
});

// ── MSG_TYPES 상수 ────────────────────────────────────────────────────────────

test('MSG_TYPES — 세 타입 정의 확인', () => {
  assert.equal(MSG_TYPES.MOTION,     'MOTION');
  assert.equal(MSG_TYPES.DOOR_FORCED,'DOOR_FORCED');
  assert.equal(MSG_TYPES.INTRUSION,  'INTRUSION');
});
