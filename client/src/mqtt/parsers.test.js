import test from 'node:test';
import assert from 'node:assert/strict';

import { parseConnectionTable } from './parsers.js';

test('parseConnectionTable preserves active and backup core ids', () => {
  const raw = JSON.stringify({
    version: 7,
    last_update: '2026-04-17T12:34:56Z',
    active_core_id: 'aaaaaaaa-1111-2222-3333-444444444444',
    backup_core_id: 'bbbbbbbb-1111-2222-3333-555555555555',
    nodes: [
      {
        id: 'aaaaaaaa-1111-2222-3333-444444444444',
        role: 'CORE',
        ip: '127.0.0.1',
        port: 1883,
        status: 'ONLINE',
        hop_to_core: 0,
      },
      {
        id: 'bbbbbbbb-1111-2222-3333-555555555555',
        role: 'CORE',
        ip: '127.0.0.1',
        port: 1884,
        status: 'ONLINE',
        hop_to_core: 1,
      },
    ],
    links: [
      {
        from_id: 'aaaaaaaa-1111-2222-3333-444444444444',
        to_id: 'bbbbbbbb-1111-2222-3333-555555555555',
        rtt_ms: 0.42,
      },
    ],
  });

  const parsed = parseConnectionTable(raw);

  assert.ok(parsed);
  assert.equal(parsed.active_core_id, 'aaaaaaaa-1111-2222-3333-444444444444');
  assert.equal(parsed.backup_core_id, 'bbbbbbbb-1111-2222-3333-555555555555');
  assert.equal(parsed.nodes.length, 2);
  assert.equal(parsed.links.length, 1);
});

test('parseConnectionTable preserves node transition metadata when present', () => {
  const raw = JSON.stringify({
    version: 9,
    last_update: '2026-04-19T06:30:00Z',
    active_core_id: 'aaaaaaaa-1111-2222-3333-444444444444',
    backup_core_id: '',
    nodes: [
      {
        id: 'dddddddd-1111-2222-3333-777777777777',
        role: 'NODE',
        ip: '192.168.0.18',
        port: 1883,
        status: 'ONLINE',
        previous_status: 'OFFLINE',
        status_changed_at: '2026-04-19T06:29:58Z',
        hop_to_core: 1,
      },
    ],
    links: [],
  });

  const parsed = parseConnectionTable(raw);

  assert.ok(parsed);
  assert.equal(parsed.nodes[0].previous_status, 'OFFLINE');
  assert.equal(parsed.nodes[0].status_changed_at, '2026-04-19T06:29:58Z');
});

test('parseConnectionTable accepts empty backup core id after promotion', () => {
  const raw = JSON.stringify({
    version: 8,
    last_update: '2026-04-17T12:35:10Z',
    active_core_id: 'bbbbbbbb-1111-2222-3333-555555555555',
    backup_core_id: '',
    nodes: [],
    links: [],
  });

  const parsed = parseConnectionTable(raw);

  assert.ok(parsed);
  assert.equal(parsed.active_core_id, 'bbbbbbbb-1111-2222-3333-555555555555');
  assert.equal(parsed.backup_core_id, '');
});

test('parseConnectionTable rejects connection tables without core ids', () => {
  const missingActive = JSON.stringify({
    version: 7,
    backup_core_id: 'bbbbbbbb-1111-2222-3333-555555555555',
    nodes: [],
    links: [],
  });
  const badBackupType = JSON.stringify({
    version: 7,
    active_core_id: 'aaaaaaaa-1111-2222-3333-444444444444',
    backup_core_id: null,
    nodes: [],
    links: [],
  });

  assert.equal(parseConnectionTable(missingActive), null);
  assert.equal(parseConnectionTable(badBackupType), null);
});
