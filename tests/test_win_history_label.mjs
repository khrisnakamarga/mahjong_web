// Unit tests for the web client's win-history label helper.
// Mirrors the historyEntryLabel function in web/app.js. If you change that
// function, mirror the change here too.

import assert from 'node:assert/strict';
import { test } from 'node:test';

function historyEntryLabel(entry, viewerSeat, seats) {
  const winds = ['East', 'South', 'West', 'North'];
  const reason = entry.reason || 'unknown';
  if (reason !== 'win') {
    const map = { exhaustive_draw: 'Exhaustive draw', exhaustiveDraw: 'Exhaustive draw' };
    return map[reason] || (entry.message || 'Round ended');
  }
  const winnerWind = (typeof entry.winnerSeat === 'number') ? winds[entry.winnerSeat] : null;
  const displayName = (Array.isArray(seats) && typeof entry.winnerSeat === 'number' && seats[entry.winnerSeat])
    ? seats[entry.winnerSeat].displayName
    : null;
  let winText;
  if (displayName && displayName !== winnerWind) winText = `${displayName} (${winnerWind})`;
  else if (winnerWind) winText = winnerWind;
  else winText = 'Winner';
  if (typeof entry.winnerSeat === 'number' && entry.winnerSeat === viewerSeat) {
    winText += ' [you]';
  }
  const src = entry.source;
  let how = '';
  if (src === 'selfDraw') how = 'self-draw';
  else if (src === 'flower') how = 'flower';
  else if (src === 'robbingKong') {
    const r = (typeof entry.responsibleSeat === 'number') ? winds[entry.responsibleSeat] : null;
    how = r ? `robbing kong (${r})` : 'robbing kong';
  } else if (src === 'discard') {
    const r = (typeof entry.responsibleSeat === 'number') ? winds[entry.responsibleSeat] : null;
    how = r ? `discard from ${r}` : 'discard';
  } else if (src) {
    how = src;
  }
  return how ? `${winText} won by ${how}` : `${winText} won`;
}

const SEATS = [
  { displayName: 'Alice' }, { displayName: 'Bob' },
  { displayName: 'Carol' }, { displayName: 'Dave' },
];

test('self-draw by viewer marks "[you]"', () => {
  const label = historyEntryLabel(
    { reason: 'win', winnerSeat: 0, source: 'selfDraw' }, 0, SEATS
  );
  assert.equal(label, 'Alice (East) [you] won by self-draw');
});

test('discard from West', () => {
  const label = historyEntryLabel(
    { reason: 'win', winnerSeat: 1, source: 'discard', responsibleSeat: 2 }, 0, SEATS
  );
  assert.equal(label, 'Bob (South) won by discard from West');
});

test('robbing kong with responsibleSeat', () => {
  const label = historyEntryLabel(
    { reason: 'win', winnerSeat: 2, source: 'robbingKong', responsibleSeat: 1 }, 2, SEATS
  );
  assert.equal(label, 'Carol (West) [you] won by robbing kong (South)');
});

test('flower win', () => {
  const label = historyEntryLabel(
    { reason: 'win', winnerSeat: 3, source: 'flower' }, 0, SEATS
  );
  assert.equal(label, 'Dave (North) won by flower');
});

test('exhaustive draw', () => {
  const label = historyEntryLabel({ reason: 'exhaustive_draw' }, 0, SEATS);
  assert.equal(label, 'Exhaustive draw');
});

test('exhaustiveDraw (camelCase variant) also resolves', () => {
  const label = historyEntryLabel({ reason: 'exhaustiveDraw' }, 0, SEATS);
  assert.equal(label, 'Exhaustive draw');
});

test('discard with missing responsibleSeat just says "discard"', () => {
  const label = historyEntryLabel(
    { reason: 'win', winnerSeat: 0, source: 'discard' }, null, SEATS
  );
  assert.equal(label, 'Alice (East) won by discard');
});

test('falls back to wind name when seats array is empty', () => {
  const label = historyEntryLabel(
    { reason: 'win', winnerSeat: 1, source: 'selfDraw' }, null, []
  );
  assert.equal(label, 'South won by self-draw');
});

test('unknown reason with message falls back to message', () => {
  const label = historyEntryLabel(
    { reason: 'forfeit', message: 'East forfeited' }, null, SEATS
  );
  assert.equal(label, 'East forfeited');
});

test('viewer marker not added when viewerSeat does not match', () => {
  const label = historyEntryLabel(
    { reason: 'win', winnerSeat: 2, source: 'selfDraw' }, 0, SEATS
  );
  assert.equal(label, 'Carol (West) won by self-draw');
});
