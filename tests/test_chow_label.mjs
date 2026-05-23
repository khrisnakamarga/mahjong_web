// Unit tests for the web client's chowLabel helper. Mirrors the logic in
// web/app.js so it can run under node --test without spinning up a browser.
// If you change chowLabel in app.js, mirror the changes here too.

import assert from 'node:assert/strict';
import { test } from 'node:test';

// Fake global state so we don't depend on the browser environment.
let state = { snapshot: null, seatIndex: 0 };

function chowLabel(a) {
  const s = state.snapshot;
  if (!s || !Array.isArray(a.tiles) || a.tiles.length !== 2) return 'Chow';
  const viewerIdx = state.seatIndex != null ? state.seatIndex : 0;
  const viewer = s.players ? s.players.find((p) => p.seatIndex === viewerIdx) : null;
  const hand = (viewer && viewer.concealedTiles) || [];
  const findById = (id) => hand.find((t) => t.id === id) || null;
  const t1 = findById(a.tiles[0]);
  const t2 = findById(a.tiles[1]);
  const claimed = s.lastDiscard && s.lastDiscard.tile && s.lastDiscard.tile.id === a.claimedTileId
    ? s.lastDiscard.tile : null;
  if (!t1 || !t2 || !claimed) return 'Chow';
  const trio = [t1, t2, claimed].slice().sort((x, y) => (x.rank || 0) - (y.rank || 0));
  const claimedKey = claimed.key;
  const parts = trio.map((t) => {
    const r = t.rank != null ? String(t.rank) : (t.key || '?');
    return t.key === claimedKey && t === trio.find((q) => q.key === claimedKey) ? `[${r}]` : r;
  });
  const suit = (claimed.suit || '').charAt(0).toUpperCase();
  return suit ? `Chow ${parts.join('·')}${suit}` : `Chow ${parts.join('·')}`;
}

const tile = (id, key, suit, rank) => ({ id, key, suit, rank });

function setupSnapshotWithChow(handTiles, discardTile) {
  state.snapshot = {
    players: [{ seatIndex: 0, concealedTiles: handTiles }],
    lastDiscard: { bySeat: 3, tile: discardTile },
  };
  state.seatIndex = 0;
}

test('chow on lower end (claimed=3, holds 4,5) → "Chow [3]·4·5B"', () => {
  setupSnapshotWithChow(
    [ tile('h1', '4b', 'bamboo', 4), tile('h2', '5b', 'bamboo', 5) ],
    tile('d1', '3b', 'bamboo', 3),
  );
  assert.equal(
    chowLabel({ type: 'chow', tiles: ['h1', 'h2'], claimedTileId: 'd1' }),
    'Chow [3]·4·5B',
  );
});

test('chow in the middle (claimed=4, holds 3,5) → "Chow 3·[4]·5B"', () => {
  setupSnapshotWithChow(
    [ tile('h1', '3b', 'bamboo', 3), tile('h2', '5b', 'bamboo', 5) ],
    tile('d1', '4b', 'bamboo', 4),
  );
  assert.equal(
    chowLabel({ type: 'chow', tiles: ['h1', 'h2'], claimedTileId: 'd1' }),
    'Chow 3·[4]·5B',
  );
});

test('chow on upper end (claimed=5, holds 3,4) → "Chow 3·4·[5]B"', () => {
  setupSnapshotWithChow(
    [ tile('h1', '3b', 'bamboo', 3), tile('h2', '4b', 'bamboo', 4) ],
    tile('d1', '5b', 'bamboo', 5),
  );
  assert.equal(
    chowLabel({ type: 'chow', tiles: ['h1', 'h2'], claimedTileId: 'd1' }),
    'Chow 3·4·[5]B',
  );
});

test('two chow options on the same discard yield DIFFERENT labels', () => {
  // Discarder plays 4-bamboo. Viewer holds {3b, 5b, 5b, 6b} — two chow options:
  //   option A: use (3b,5b) for 3-4-5
  //   option B: use (5b,6b) for 4-5-6
  setupSnapshotWithChow(
    [ tile('h1', '3b', 'bamboo', 3),
      tile('h2', '5b', 'bamboo', 5),
      tile('h3', '5b', 'bamboo', 5),
      tile('h4', '6b', 'bamboo', 6) ],
    tile('d1', '4b', 'bamboo', 4),
  );
  const lblA = chowLabel({ type: 'chow', tiles: ['h1', 'h2'], claimedTileId: 'd1' });
  const lblB = chowLabel({ type: 'chow', tiles: ['h3', 'h4'], claimedTileId: 'd1' });
  assert.equal(lblA, 'Chow 3·[4]·5B');
  assert.equal(lblB, 'Chow [4]·5·6B');
  assert.notEqual(lblA, lblB,
    'Two chow options on the same discard must be distinguishable in the UI');
});

test('character suit produces "Chow ...C"', () => {
  setupSnapshotWithChow(
    [ tile('h1', '7c', 'character', 7), tile('h2', '8c', 'character', 8) ],
    tile('d1', '6c', 'character', 6),
  );
  assert.equal(
    chowLabel({ type: 'chow', tiles: ['h1', 'h2'], claimedTileId: 'd1' }),
    'Chow [6]·7·8C',
  );
});

test('dot suit produces "Chow ...D"', () => {
  setupSnapshotWithChow(
    [ tile('h1', '1d', 'dot', 1), tile('h2', '2d', 'dot', 2) ],
    tile('d1', '3d', 'dot', 3),
  );
  assert.equal(
    chowLabel({ type: 'chow', tiles: ['h1', 'h2'], claimedTileId: 'd1' }),
    'Chow 1·2·[3]D',
  );
});

test('missing snapshot → "Chow" (safe fallback)', () => {
  state.snapshot = null;
  assert.equal(
    chowLabel({ type: 'chow', tiles: ['h1', 'h2'], claimedTileId: 'd1' }),
    'Chow',
  );
});

test('missing claimed tile → "Chow" (safe fallback)', () => {
  state.snapshot = {
    players: [{ seatIndex: 0, concealedTiles: [
      tile('h1', '4b', 'bamboo', 4), tile('h2', '5b', 'bamboo', 5),
    ] }],
    lastDiscard: { bySeat: 3, tile: tile('d_other', '9c', 'character', 9) },
  };
  state.seatIndex = 0;
  assert.equal(
    chowLabel({ type: 'chow', tiles: ['h1', 'h2'], claimedTileId: 'd1' }),
    'Chow',
  );
});

test('action with wrong shape → "Chow" (safe fallback)', () => {
  setupSnapshotWithChow(
    [ tile('h1', '4b', 'bamboo', 4), tile('h2', '5b', 'bamboo', 5) ],
    tile('d1', '3b', 'bamboo', 3),
  );
  assert.equal(chowLabel({ type: 'chow' }), 'Chow');
  assert.equal(chowLabel({ type: 'chow', tiles: ['h1'], claimedTileId: 'd1' }), 'Chow');
});
