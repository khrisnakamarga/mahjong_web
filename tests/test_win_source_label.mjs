// Unit tests for the web client's winSourceLabel helper.
// We isolate the same logic that web/app.js uses so we can verify it without
// spinning up a browser. If you change winSourceLabel in app.js, mirror it
// here. The test cases cover self-draw, discard with a known discarder,
// discard with missing lastDiscard, flower, robbing-kong, and absent source.

import assert from 'node:assert/strict';
import { test } from 'node:test';

function winSourceLabel(s) {
  const c = s.conclusion;
  if (!c || !c.source) return '';
  const winds = ['East', 'South', 'West', 'North'];
  if (c.source === 'selfDraw') return 'Won by self-draw';
  if (c.source === 'discard') {
    const bySeat = (typeof c.responsibleSeat === 'number')
      ? c.responsibleSeat
      : (s.lastDiscard ? s.lastDiscard.bySeat : null);
    const seatName = (bySeat != null && bySeat >= 0 && bySeat < 4) ? winds[bySeat] : null;
    return seatName ? `Won on discard from ${seatName}` : 'Won on discard';
  }
  if (c.source === 'flower') return 'Won by flower';
  if (c.source === 'robbingKong') return 'Won by robbing kong';
  return `Won (${c.source})`;
}

test('selfDraw → "Won by self-draw"', () => {
  const label = winSourceLabel({ conclusion: { source: 'selfDraw' } });
  assert.equal(label, 'Won by self-draw');
});

test('discard with bySeat=0 (East) → "Won on discard from East"', () => {
  const label = winSourceLabel({
    conclusion: { source: 'discard' },
    lastDiscard: { bySeat: 0 },
  });
  assert.equal(label, 'Won on discard from East');
});

test('discard with bySeat=1 (South)', () => {
  const label = winSourceLabel({
    conclusion: { source: 'discard' },
    lastDiscard: { bySeat: 1 },
  });
  assert.equal(label, 'Won on discard from South');
});

test('discard with bySeat=2 (West)', () => {
  const label = winSourceLabel({
    conclusion: { source: 'discard' },
    lastDiscard: { bySeat: 2 },
  });
  assert.equal(label, 'Won on discard from West');
});

test('discard with bySeat=3 (North)', () => {
  const label = winSourceLabel({
    conclusion: { source: 'discard' },
    lastDiscard: { bySeat: 3 },
  });
  assert.equal(label, 'Won on discard from North');
});

test('discard with missing lastDiscard → "Won on discard" (no source seat)', () => {
  const label = winSourceLabel({ conclusion: { source: 'discard' } });
  assert.equal(label, 'Won on discard');
});

test('flower → "Won by flower"', () => {
  const label = winSourceLabel({ conclusion: { source: 'flower' } });
  assert.equal(label, 'Won by flower');
});

test('robbingKong → "Won by robbing kong"', () => {
  const label = winSourceLabel({ conclusion: { source: 'robbingKong' } });
  assert.equal(label, 'Won by robbing kong');
});

test('exhaustive draw (no source) → "" (no label shown)', () => {
  const label = winSourceLabel({ conclusion: { reason: 'exhaustiveDraw' } });
  assert.equal(label, '');
});

test('no conclusion → ""', () => {
  assert.equal(winSourceLabel({}), '');
  assert.equal(winSourceLabel({ conclusion: null }), '');
});

test('discard with out-of-range bySeat → "Won on discard"', () => {
  const label = winSourceLabel({
    conclusion: { source: 'discard' },
    lastDiscard: { bySeat: 99 },
  });
  assert.equal(label, 'Won on discard');
});

test('discard uses conclusion.responsibleSeat when lastDiscard is missing', () => {
  // Server clears lastDiscard at finish(), but sets responsibleSeat on
  // discard wins so the client can still render the discarder's wind.
  const label = winSourceLabel({
    conclusion: { source: 'discard', responsibleSeat: 2 },
  });
  assert.equal(label, 'Won on discard from West');
});

test('discard prefers conclusion.responsibleSeat over lastDiscard.bySeat', () => {
  const label = winSourceLabel({
    conclusion: { source: 'discard', responsibleSeat: 1 },
    lastDiscard: { bySeat: 0 },
  });
  assert.equal(label, 'Won on discard from South');
});

test('discard falls back to lastDiscard when responsibleSeat is undefined', () => {
  const label = winSourceLabel({
    conclusion: { source: 'discard' },
    lastDiscard: { bySeat: 3 },
  });
  assert.equal(label, 'Won on discard from North');
});
