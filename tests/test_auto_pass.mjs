// Unit tests for the web client's auto-pass / auto-draw logic.
//
// We can't run the full browser environment from node, but we can extract the
// pure decision function and exercise it with synthetic snapshots. Run with:
//
//     node tests\test_auto_pass.mjs
//
// The test verifies that:
//   1. Auto-draw fires when the only legal action is `draw`.
//   2. Auto-pass fires when the toggle is on AND pass is offered AND no `win`.
//   3. Auto-pass fires when offered an optional chow/pong/kong + pass.
//   4. Auto-pass does NOT fire when a win is on the table.
//   5. Auto-pass does NOT fire when the toggle is off.
//   6. Auto-pass debounces consecutive calls within the cooldown window.

import assert from 'node:assert/strict';

// Inline copy of the decision logic from web/app.js. Keep in sync.
function makeAutoActor(state, sendAction) {
  return function maybeAutoAct() {
    const actions = state.snapshot && state.seatIndex != null
        ? (state.snapshot.legalActions || []) : [];
    if (!actions.length) return;
    if (!state.snapshot) return;
    if (state.lastAutoActVersion === state.snapshot.version) return;
    if (actions.length === 1 && actions[0].type === 'draw') {
      state.lastAutoActVersion = state.snapshot.version;
      sendAction(actions[0]);
      return;
    }
    if (state.autoPass) {
      const passAction = actions.find((a) => a.type === 'pass');
      const hasCallOption = actions.some((a) =>
          a.type === 'win' || a.type === 'chow' || a.type === 'pong' || a.type === 'kong');
      if (passAction && !hasCallOption) {
        state.lastAutoActVersion = state.snapshot.version;
        sendAction(passAction);
      }
    }
  };
}

function makeState(overrides = {}) {
  return {
    seatIndex: 0,
    snapshot: { version: 1, legalActions: [] },
    autoPass: false,
    lastAutoActVersion: -1,
    ...overrides,
  };
}

function captureSends() {
  const sent = [];
  return {
    sendAction: (a) => sent.push(a),
    sent,
  };
}

function test(label, fn) {
  try { fn(); console.log(`  ok   ${label}`); }
  catch (err) { console.error(`  FAIL ${label}\n       ${err.message}`); process.exitCode = 1; }
}

console.log('test_auto_pass.mjs');

test('auto-draw fires when only action is draw', () => {
  const state = makeState({ snapshot: { version: 1, legalActions: [{ type: 'draw' }] } });
  const c = captureSends();
  makeAutoActor(state, c.sendAction)();
  assert.deepEqual(c.sent.map((a) => a.type), ['draw']);
  assert.equal(state.lastAutoActVersion, 1);
});

test('auto-pass fires when pass is the only action and toggle is on', () => {
  const state = makeState({
    autoPass: true,
    snapshot: { version: 1, legalActions: [{ type: 'pass' }] },
  });
  const c = captureSends();
  makeAutoActor(state, c.sendAction)();
  assert.deepEqual(c.sent.map((a) => a.type), ['pass']);
});

test('auto-pass does NOT fire when offered an optional chow + pass (user must decide)', () => {
  const state = makeState({
    autoPass: true,
    snapshot: { version: 1, legalActions: [
      { type: 'chow', tileId: 't-1', tiles: ['t-2', 't-3'] },
      { type: 'pass' },
    ]},
  });
  const c = captureSends();
  makeAutoActor(state, c.sendAction)();
  assert.deepEqual(c.sent, []);
});

test('auto-pass does NOT fire when offered an optional pong + pass (user must decide)', () => {
  const state = makeState({
    autoPass: true,
    snapshot: { version: 1, legalActions: [
      { type: 'pong', tileId: 't-1' },
      { type: 'pass' },
    ]},
  });
  const c = captureSends();
  makeAutoActor(state, c.sendAction)();
  assert.deepEqual(c.sent, []);
});

test('auto-pass does NOT fire when offered an optional kong + pass (user must decide)', () => {
  const state = makeState({
    autoPass: true,
    snapshot: { version: 1, legalActions: [
      { type: 'kong', tileId: 't-1', kongType: 'exposed' },
      { type: 'pass' },
    ]},
  });
  const c = captureSends();
  makeAutoActor(state, c.sendAction)();
  assert.deepEqual(c.sent, []);
});

test('auto-pass does NOT fire when win is on the table', () => {
  const state = makeState({
    autoPass: true,
    snapshot: { version: 1, legalActions: [
      { type: 'win', tileId: 't-1', source: 'discard' },
      { type: 'pass' },
    ]},
  });
  const c = captureSends();
  makeAutoActor(state, c.sendAction)();
  assert.deepEqual(c.sent, []);
});

test('auto-pass does NOT fire when toggle is off', () => {
  const state = makeState({
    autoPass: false,
    snapshot: { version: 1, legalActions: [
      { type: 'pass' },
    ]},
  });
  const c = captureSends();
  makeAutoActor(state, c.sendAction)();
  assert.deepEqual(c.sent, []);
});

test('auto-pass does NOT fire when chow+pong+pass are offered (multiple call options)', () => {
  const state = makeState({
    autoPass: true,
    snapshot: { version: 1, legalActions: [
      { type: 'chow', tileId: 't-1' },
      { type: 'pong', tileId: 't-2' },
      { type: 'pass' },
    ]},
  });
  const c = captureSends();
  makeAutoActor(state, c.sendAction)();
  assert.deepEqual(c.sent, []);
});

test('auto-pass debounces within a single snapshot version', () => {
  const state = makeState({
    autoPass: true,
    snapshot: { version: 1, legalActions: [{ type: 'pass' }] },
  });
  const c = captureSends();
  const fn = makeAutoActor(state, c.sendAction);
  fn(); fn(); fn();
  assert.deepEqual(c.sent.length, 1);
});

test('auto-pass FIRES AGAIN on new snapshot version (regression: time-based debounce deadlock)', () => {
  // This is the original bug: if the server sends snapshot v=8 -> v=13 quickly
  // (both with [pass] for our seat, because the claim window is still open),
  // the old time-based debounce would skip v=13 because pendingAutoPass was
  // still true. Version-keyed debounce must allow v=13 to fire.
  const state = makeState({
    autoPass: true,
    snapshot: { version: 8, legalActions: [{ type: 'pass' }] },
  });
  const c = captureSends();
  const fn = makeAutoActor(state, c.sendAction);
  fn(); // sends pass for v=8
  assert.deepEqual(c.sent.length, 1);
  // Server broadcasts v=13 (claim window still open, still our seat to pass).
  state.snapshot = { version: 13, legalActions: [{ type: 'pass' }] };
  fn(); // MUST send pass for v=13
  assert.deepEqual(c.sent.length, 2, 'auto-pass deadlocked: v=13 was skipped');
});

test('auto-pass fires even when only choice is pass', () => {
  const state = makeState({
    autoPass: true,
    snapshot: { version: 1, legalActions: [{ type: 'pass' }] },
  });
  const c = captureSends();
  makeAutoActor(state, c.sendAction)();
  assert.deepEqual(c.sent.map((a) => a.type), ['pass']);
});

if (process.exitCode) {
  console.error('\nFAILED');
} else {
  console.log('\nall tests passed');
}
