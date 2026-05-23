// Browser client for the Hong Kong Mahjong server.
// Connects via WebSocket, renders snapshots on a Canvas, and submits actions
// when the user clicks tiles / action buttons.

const $ = (id) => document.getElementById(id);

const state = {
  ws: null,
  roomCode: null,
  seatIndex: null,
  sessionToken: null,
  snapshot: null,
  hoverTileId: null,
  autoPass: false,
  // Mirrors snapshot.minFan; null until first snapshot.
  minFan: null,
  // Version-keyed debounce: the highest snapshot.version for which we've
  // already submitted an auto-action. Prevents resending while we wait for
  // the server's acknowledgement snapshot, but does NOT block legitimate
  // future snapshots (unlike a time-based debounce, which can deadlock if
  // the next snapshot arrives before the timer fires).
  lastAutoActVersion: -1,
  aiDelayMs: 0,
  // Pixel hitboxes for the current frame: each is {tileId, kind, x, y, w, h}
  hitTargets: [],
  // ---------- Lobby seat picker state ----------
  // After createRoom: array of {seatIndex, token, url} (one per seat). When
  // the local user has a private claim link via URL params, the matching
  // entry is also stored here so the seat card can render "Join as ...".
  lobbyTokens: {},      // map seatIndex -> token (string)
  // Most recent snapshot fetched via GET /api/rooms/<code> for occupancy.
  lobbySnapshot: null,
  lobbyRoomCode: null,  // room code currently being polled in the lobby
  lobbyPollTimer: null, // setInterval handle
};
// Expose for debugging / smoke tests.
if (typeof window !== 'undefined') window.state = state;

// ---------- API helpers ----------
async function apiCreateRoom() {
  const r = await fetch('/api/rooms', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' });
  if (!r.ok) throw new Error('create_room failed: ' + r.status);
  return await r.json();
}
async function apiClaimSeat(code, seat, token, displayName) {
  const r = await fetch(`/api/rooms/${code}/seats/${seat}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ token, displayName }),
  });
  const body = await r.json();
  if (!r.ok) throw new Error(body.message || 'claim failed');
  return body;
}

// ---------- Lobby UI ----------
const SEAT_LABELS = ['East', 'South', 'West', 'North'];

async function apiGetRoom(code) {
  try {
    const r = await fetch('/api/rooms/' + encodeURIComponent(code));
    if (!r.ok) return null;
    return await r.json();
  } catch (_) { return null; }
}

// Stop the background poll that refreshes lobby seat occupancy.
function stopLobbyPolling() {
  if (state.lobbyPollTimer) {
    clearInterval(state.lobbyPollTimer);
    state.lobbyPollTimer = null;
  }
  state.lobbyRoomCode = null;
  state.lobbySnapshot = null;
}

// Begin polling room snapshot every 3s so the lobby seat cards reflect new
// joins in near-real-time. Always re-renders both seat grids on success.
function startLobbyPolling(code) {
  stopLobbyPolling();
  state.lobbyRoomCode = code;
  const tick = async () => {
    if (state.lobbyRoomCode !== code) return;
    const snap = await apiGetRoom(code);
    if (state.lobbyRoomCode !== code) return;
    if (snap) {
      state.lobbySnapshot = snap;
      renderSeatGrids();
    }
  };
  tick();
  state.lobbyPollTimer = setInterval(tick, 3000);
}

// Render one seat card -- shared between the "Create" and "Join" grids.
// `opts.ownTokens` is a map seatIndex->token of tokens the local user owns;
// seats in this map render a primary "Join as <wind>" button. `opts.context`
// is 'create' or 'join' (used to differentiate copy-link visibility).
function renderSeatCard(seatIndex, snapshot, opts) {
  const seat = snapshot && snapshot.seats ? snapshot.seats[seatIndex] : null;
  const label = SEAT_LABELS[seatIndex] || `Seat ${seatIndex}`;
  const taken = seat && seat.controller === 'human';
  const ownToken = opts.ownTokens ? opts.ownTokens[seatIndex] : null;
  const isYou = !!ownToken;

  const card = document.createElement('div');
  card.className = 'seatCard ' + (taken && !isYou ? 'taken' : 'open') + (isYou ? ' yours' : '');
  card.dataset.seatIndex = String(seatIndex);

  const header = document.createElement('div');
  header.className = 'seatHeader';
  const lbl = document.createElement('span');
  lbl.className = 'seatLabel';
  lbl.textContent = `${label} (seat ${seatIndex})`;
  const badge = document.createElement('span');
  badge.className = 'seatBadge';
  if (taken) badge.textContent = isYou ? 'Your seat' : 'Taken';
  else badge.textContent = isYou ? 'Your seat' : 'Open';
  header.append(lbl, badge);

  const status = document.createElement('div');
  status.className = 'seatStatus';
  if (!snapshot) {
    status.textContent = 'Loading...';
  } else if (taken) {
    const name = seat.displayName || 'Player';
    const conn = seat.connected ? 'connected' : 'disconnected';
    status.textContent = isYou ? `You're seated here (${conn})` : `Taken by ${name} (${conn})`;
  } else {
    status.textContent = 'AI is filling this seat. Click "Join as ' + label + '" to take it.';
  }

  const actions = document.createElement('div');
  actions.className = 'seatActions';

  // Primary action: a "Join as <wind>" button. Enabled if:
  //   - The local user owns this seat's token (from create flow or URL), AND
  //   - The seat is not taken by a different human.
  // Disabled (visible but greyed-out) otherwise so the affordance is obvious.
  if (ownToken) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'seatJoin';
    btn.textContent = `Join as ${label}`;
    btn.disabled = taken && !isYou;
    btn.addEventListener('click', () => joinAsSeat(seatIndex, ownToken));
    actions.appendChild(btn);
  } else if (!taken) {
    // No token: cannot join. Surface a hint instead of a button.
    const note = document.createElement('span');
    note.className = 'seatStatus';
    note.style.fontStyle = 'italic';
    note.textContent = 'Needs invite link';
    actions.appendChild(note);
  }

  // Copy-link button (Create flow only) so the host can DM the seat URL to a
  // friend. The URL embeds the seat's secret token and is the only way a
  // remote player can claim that seat.
  if (opts.context === 'create' && ownToken) {
    const claimLink = (state.lobbyClaimLinks || []).find((l) => l.seatIndex === seatIndex);
    const url = claimLink ? claimLink.url
      : `${location.origin}/?room=${state.lobbyRoomCode}&seat=${seatIndex}&token=${ownToken}`;
    const copy = document.createElement('button');
    copy.type = 'button';
    copy.className = 'secondary';
    copy.style.flex = '0 0 auto';
    copy.style.margin = '0';
    copy.textContent = 'Copy link';
    copy.title = url;
    copy.addEventListener('click', () => {
      navigator.clipboard.writeText(url).then(
        () => { copy.textContent = 'Copied'; setTimeout(() => { copy.textContent = 'Copy link'; }, 1500); },
        () => alert('Clipboard failed -- URL:\n' + url)
      );
    });
    actions.appendChild(copy);
  }

  card.append(header, status, actions);
  return card;
}

// Refresh both lobby seat grids based on the latest polled snapshot. Called
// after each successful poll and whenever the user types a room code.
function renderSeatGrids() {
  const snap = state.lobbySnapshot;
  // ---- Create flow grid (shown after createRoom; host owns all 4 tokens).
  const createGrid = $('seatGrid');
  if (createGrid && !$('createRoomResult').classList.contains('hidden')) {
    createGrid.innerHTML = '';
    for (let i = 0; i < 4; i++) {
      createGrid.appendChild(renderSeatCard(i, snap, {
        ownTokens: state.lobbyTokens,
        context: 'create',
      }));
    }
  }
  // ---- Join flow grid (shown when user types a valid room code).
  const joinGrid = $('joinSeatGrid');
  const joinHint = $('joinSeatHint');
  if (joinGrid && !joinGrid.classList.contains('hidden')) {
    joinGrid.innerHTML = '';
    // For join flow we trust the URL params to indicate which seat the
    // viewer owns. If they don't have a token, no seat is "yours".
    const params = new URLSearchParams(location.search);
    const urlToken = params.get('token');
    const urlSeat = params.get('seat');
    const ownTokens = {};
    if (urlToken && urlSeat != null && state.lobbyRoomCode &&
        params.get('room') && params.get('room').toUpperCase() === state.lobbyRoomCode) {
      const seatIdx = parseInt(urlSeat, 10);
      if (Number.isInteger(seatIdx) && seatIdx >= 0 && seatIdx <= 3) {
        ownTokens[seatIdx] = urlToken;
      }
    }
    for (let i = 0; i < 4; i++) {
      joinGrid.appendChild(renderSeatCard(i, snap, { ownTokens, context: 'join' }));
    }
    if (joinHint) {
      if (Object.keys(ownTokens).length === 0) {
        joinHint.textContent = 'Open the private seat link a host shared with you to claim a seat, or click "Spectate only".';
        joinHint.classList.remove('hidden');
      } else {
        joinHint.classList.add('hidden');
      }
    }
  }
}

// Claim a seat (called from a seat card's primary button). Resolves the
// display name from whichever input is visible, calls the existing claim
// API, then transitions into the table view via enterRoom().
async function joinAsSeat(seatIndex, token) {
  const code = state.lobbyRoomCode;
  if (!code) { alert('No room selected.'); return; }
  // Prefer the input belonging to the currently visible card section.
  const createName = $('createDisplayName');
  const joinName = $('joinDisplayName');
  const fromCreate = createName && !$('createRoomResult').classList.contains('hidden');
  let name = '';
  if (fromCreate) name = (createName.value || '').trim();
  if (!name && joinName) name = (joinName.value || '').trim();
  if (!name) name = SEAT_LABELS[seatIndex] || 'Player';
  try {
    const claim = await apiClaimSeat(code, seatIndex, token, name);
    stopLobbyPolling();
    enterRoom(claim.roomCode, seatIndex, claim.sessionToken);
  } catch (e) {
    alert('Join failed: ' + (e && e.message ? e.message : e));
    // Refresh the grid so it shows the new occupancy state.
    apiGetRoom(code).then((snap) => { if (snap) { state.lobbySnapshot = snap; renderSeatGrids(); } });
  }
}

function setupLobby() {
  $('createRoomBtn').addEventListener('click', async () => {
    try {
      const data = await apiCreateRoom();
      $('createRoomResult').classList.remove('hidden');
      $('newRoomCode').textContent = data.roomCode;
      // Stash everything we need to render seat cards + drive the polling.
      state.lobbyRoomCode = data.roomCode;
      state.lobbyClaimLinks = data.claimLinks || [];
      state.lobbyTokens = {};
      for (const link of state.lobbyClaimLinks) {
        state.lobbyTokens[link.seatIndex] = link.token;
      }
      // Immediate render with no snapshot yet (cards will show "Loading...");
      // poller below will populate occupancy in a few hundred ms.
      renderSeatGrids();
      startLobbyPolling(data.roomCode);
    } catch (e) {
      alert('Failed to create room: ' + e.message);
    }
  });

  // Live-fetch the join seat grid whenever the user types a 6-char code.
  const roomCodeInput = $('joinRoomCode');
  let joinLookupTimer = null;
  const refreshJoinSeats = () => {
    const code = (roomCodeInput.value || '').trim().toUpperCase();
    const joinGrid = $('joinSeatGrid');
    const joinHint = $('joinSeatHint');
    if (code.length < 4) {
      joinGrid.classList.add('hidden');
      if (joinHint) joinHint.classList.add('hidden');
      // Only stop polling if we're not in the middle of a create flow.
      if (!$('createRoomResult') || $('createRoomResult').classList.contains('hidden')) {
        stopLobbyPolling();
      }
      return;
    }
    // Don't fight the create-flow poller if it's already pointed at the
    // same code (the user might've pasted their own room code).
    joinGrid.classList.remove('hidden');
    const createOpen = $('createRoomResult') && !$('createRoomResult').classList.contains('hidden');
    if (!createOpen || state.lobbyRoomCode !== code) {
      startLobbyPolling(code);
    } else {
      renderSeatGrids();
    }
  };
  roomCodeInput.addEventListener('input', () => {
    if (joinLookupTimer) clearTimeout(joinLookupTimer);
    joinLookupTimer = setTimeout(refreshJoinSeats, 400);
  });
  roomCodeInput.addEventListener('blur', refreshJoinSeats);

  $('joinRoomBtn').addEventListener('click', async () => {
    const code = $('joinRoomCode').value.trim().toUpperCase();
    const seat = parseInt($('joinSeatIndex').value, 10);
    const token = $('joinSeatToken').value.trim();
    const name = $('joinDisplayName').value.trim() || 'Player';
    if (!code || !token || isNaN(seat)) { alert('Room code, seat index, and seat token are required.'); return; }
    try {
      const claim = await apiClaimSeat(code, seat, token, name);
      stopLobbyPolling();
      enterRoom(claim.roomCode, seat, claim.sessionToken);
    } catch (e) {
      alert('Join failed: ' + e.message);
    }
  });
  $('spectateBtn').addEventListener('click', () => {
    const code = $('joinRoomCode').value.trim().toUpperCase();
    if (!code) { alert('Enter a room code to spectate.'); return; }
    stopLobbyPolling();
    enterRoom(code, null, null);
  });
  $('leaveBtn').addEventListener('click', () => {
    if (state.ws) { state.ws.close(); state.ws = null; }
    state.roomCode = null;
    state.seatIndex = null;
    state.sessionToken = null;
    state.snapshot = null;
    $('table').classList.add('hidden');
    $('lobby').classList.remove('hidden');
    history.replaceState(null, '', location.pathname);
  });
  $('autoAiToggle').addEventListener('change', (e) => {
    const value = e.target.checked;
    state.autoPass = value;
    // Persist to the room so every connected client (and the toggle on
    // subsequent reconnects) sees the same value. The server broadcasts a
    // fresh snapshot with autoPass=value so peers update their UI.
    if (state.ws && state.ws.readyState === WebSocket.OPEN && state.roomCode) {
      state.ws.send(JSON.stringify({ type: 'set_auto_pass', value }));
    }
    maybeAutoAct();
  });
  // Chicken Hand toggle. ON => minFan=0 (any winning hand pays);
  // OFF => minFan=3 (standard HK Mahjong default). Room-scoped: every
  // connected client sees the same value via snapshot.minFan.
  const chickenToggle = $('chickenHandToggle');
  if (chickenToggle) {
    chickenToggle.addEventListener('change', (e) => {
      const value = e.target.checked ? 0 : 3;
      state.minFan = value;
      if (state.ws && state.ws.readyState === WebSocket.OPEN && state.roomCode) {
        state.ws.send(JSON.stringify({ type: 'set_min_fan', value }));
      }
    });
  }
  // AI speed slider. Sends a set_ai_delay message to the server; the server
  // broadcasts a fresh snapshot containing the updated aiDelayMs so all
  // clients see the value sync.
  $('aiSpeedSlider').addEventListener('input', (e) => {
    const ms = parseInt(e.target.value, 10) || 0;
    state.aiDelayMs = ms;
    $('aiSpeedValue').textContent = (ms / 1000).toFixed(1) + 's';
    if (state.ws && state.ws.readyState === WebSocket.OPEN && state.roomCode) {
      state.ws.send(JSON.stringify({ type: 'set_ai_delay', delayMs: ms }));
    }
  });

  // History panel toggle. Lets the user reclaim screen space on phones.
  // On large screens the panel is visible by default; on small screens we
  // start with it hidden (CSS sets the right default via .userHidden).
  const histBtn = $('toggleHistoryBtn');
  if (histBtn) {
    histBtn.addEventListener('click', () => {
      state.userHidWinHistory = !state.userHidWinHistory;
      histBtn.setAttribute('aria-pressed', state.userHidWinHistory ? 'false' : 'true');
      // Re-render so renderWinHistory reapplies the visibility decision.
      if (state.snapshot) renderWinHistory(state.snapshot);
    });
    // Default: hide on phones (<=600px viewport), show on tablets/desktop.
    state.userHidWinHistory = window.matchMedia('(max-width: 600px)').matches;
    histBtn.setAttribute('aria-pressed', state.userHidWinHistory ? 'false' : 'true');
  }

  // Rotate-device overlay dismiss button. Once dismissed we add a class that
  // the CSS rule checks; the user can rotate back/forth without it returning.
  const rotate = $('rotateOverlay');
  const dismissBtn = $('dismissRotateBtn');
  if (rotate && dismissBtn) {
    if (sessionStorage.getItem('mj_rotate_dismissed') === '1') rotate.classList.add('dismissed');
    dismissBtn.addEventListener('click', () => {
      rotate.classList.add('dismissed');
      try { sessionStorage.setItem('mj_rotate_dismissed', '1'); } catch (_) {}
    });
  }

  // If the page was opened from a private seat link, prefill the advanced
  // fields AND auto-show the join seat grid pointed at that room so the user
  // can confirm their seat with one click.
  const params = new URLSearchParams(location.search);
  if (params.get('room') && params.get('token')) {
    $('joinRoomCode').value = params.get('room').toUpperCase();
    $('joinSeatIndex').value = params.get('seat') || '0';
    $('joinSeatToken').value = params.get('token');
    refreshJoinSeats();
  }
}

// ---------- WebSocket ----------
function enterRoom(roomCode, seatIndex, sessionToken) {
  state.roomCode = roomCode;
  state.seatIndex = seatIndex;
  state.sessionToken = sessionToken;
  $('lobby').classList.add('hidden');
  $('table').classList.remove('hidden');
  $('statusText').textContent = 'Connecting…';

  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(`${proto}//${location.host}/ws`);
  state.ws = ws;

  ws.addEventListener('open', () => {
    const hello = { type: 'hello', roomCode };
    if (sessionToken && seatIndex != null) {
      hello.seatIndex = seatIndex;
      hello.sessionToken = sessionToken;
    }
    ws.send(JSON.stringify(hello));
  });
  ws.addEventListener('message', (ev) => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }
    if (msg.type === 'welcome') {
      $('statusText').textContent = `Room ${msg.roomCode}` + (msg.seatIndex != null ? ` · seat ${msg.seatIndex}` : ' · spectator');
    } else if (msg.type === 'snapshot') {
      onSnapshot(msg.snapshot);
    } else if (msg.type === 'error') {
      console.warn('server error', msg);
      flashError(`${msg.code}: ${msg.message}`);
    } else if (msg.type === 'pong') {
      // ok
    }
  });
  ws.addEventListener('close', () => {
    $('statusText').textContent = 'Disconnected';
  });
  // keepalive ping
  setInterval(() => {
    if (state.ws && state.ws.readyState === WebSocket.OPEN) state.ws.send('{"type":"ping"}');
  }, 30000);
}

function flashError(message) {
  const el = $('statusText');
  const prev = el.textContent;
  el.style.color = '#ffb4b4';
  el.textContent = '⚠ ' + message;
  setTimeout(() => { el.style.color = ''; el.textContent = prev; }, 3000);
}

function sendAction(action) {
  if (!state.ws || state.ws.readyState !== WebSocket.OPEN) return;
  state.ws.send(JSON.stringify({
    type: 'action',
    expectedVersion: state.snapshot ? state.snapshot.version : 0,
    action,
  }));
}

// ---------- Snapshot handling ----------
function onSnapshot(snapshot) {
  state.snapshot = snapshot;
  // Sync slider display to whatever the server has authoritatively recorded
  // for this room. Avoids drift if multiple browsers in the same room move
  // the slider — last writer wins, but everyone sees the same value.
  if (typeof snapshot.aiDelayMs === 'number' && snapshot.aiDelayMs !== state.aiDelayMs) {
    state.aiDelayMs = snapshot.aiDelayMs;
    const slider = $('aiSpeedSlider');
    const valueEl = $('aiSpeedValue');
    if (slider) slider.value = String(snapshot.aiDelayMs);
    if (valueEl) valueEl.textContent = (snapshot.aiDelayMs / 1000).toFixed(1) + 's';
  }
  // Sync the room-scoped autoPass toggle. Server is the source of truth so
  // every client in the room reflects the latest value.
  if (typeof snapshot.autoPass === 'boolean' && snapshot.autoPass !== state.autoPass) {
    state.autoPass = snapshot.autoPass;
    const toggle = $('autoAiToggle');
    if (toggle) toggle.checked = snapshot.autoPass;
  }
  // Sync the room-scoped Chicken Hand toggle (minFan). 0 => chicken on,
  // anything else (typically 3) => chicken off.
  if (typeof snapshot.minFan === 'number' && snapshot.minFan !== state.minFan) {
    state.minFan = snapshot.minFan;
    const chickenToggle = $('chickenHandToggle');
    if (chickenToggle) chickenToggle.checked = snapshot.minFan === 0;
  }
  updateStatusFromSnapshot();
  renderActionButtons();
  draw();
  maybeAutoAct();
}

function ownLegalActions() {
  if (!state.snapshot || state.seatIndex == null) return [];
  return state.snapshot.legalActions || [];
}

function maybeAutoAct() {
  const actions = ownLegalActions();
  if (!actions.length) return;
  if (!state.snapshot) return;
  // Only auto-act once per snapshot version. The server will broadcast a new
  // snapshot with a higher version after our action lands, which re-enables
  // this guard for subsequent windows.
  if (state.lastAutoActVersion === state.snapshot.version) return;

  // Auto-draw: drawing is never strategic.
  if (actions.length === 1 && actions[0].type === 'draw') {
    state.lastAutoActVersion = state.snapshot.version;
    sendAction(actions[0]);
    return;
  }
  // Auto-pass when toggle is on. Only fires when Pass is the ONLY meaningful
  // action available — i.e., no claim options (chow / pong / kong / win) are
  // on the table. As soon as the user can call a tile, auto-pass is
  // interrupted so the user can decide whether to claim or pass manually.
  if (state.autoPass) {
    const passAction = actions.find((a) => a.type === 'pass');
    const hasCallOption = actions.some((a) =>
        a.type === 'win' || a.type === 'chow' || a.type === 'pong' || a.type === 'kong');
    if (passAction && !hasCallOption) {
      state.lastAutoActVersion = state.snapshot.version;
      sendAction(passAction);
    }
  }
}

function updateStatusFromSnapshot() {
  const s = state.snapshot;
  if (!s) return;
  const winds = ['East', 'South', 'West', 'North'];
  let label = `Room ${s.roomCode}`;
  if (state.seatIndex != null) label += ` · seat ${winds[state.seatIndex]}`;
  else label += ' · spectator';
  $('statusText').textContent = label;
  // Conclusion banner
  const conc = $('conclusion');
  if (s.conclusion) {
    conc.classList.remove('hidden');
    let html = `<h3>${s.conclusion.message || 'Round complete'}</h3>`;
    const srcText = winSourceLabel(s);
    if (srcText) html += `<p class="winSource">${srcText}</p>`;
    if (s.conclusion.winningTile) {
      const t = s.conclusion.winningTile;
      const label = (t.tileKey || t.id || '').replace(/[#].*$/, '').replace(/-/g, ' ');
      html += `<p class="winTile">Winning tile: <strong>${label}</strong></p>`;
    }
    if (s.conclusion.settlement) {
      const set = s.conclusion.settlement;
      html += `<p>${set.fan} fan · base ${set.basePoints}</p>`;
      html += '<ul>' + (set.paymentLines || []).map((l) => `<li>${l.from} → ${l.to}: ${l.points}</li>`).join('') + '</ul>';
    }
    conc.innerHTML = html;
  } else {
    conc.classList.add('hidden');
    conc.innerHTML = '';
  }
  // Win history panel: render the room's append-only conclusion log.
  renderWinHistory(s);
}

// ---------- Win history ----------
function tileDisplayName(t) {
  if (!t) return '';
  return (t.tileKey || t.id || '').replace(/[#].*$/, '').replace(/-/g, ' ');
}

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

function renderWinHistory(s) {
  const panel = $('winHistory');
  if (!panel) return;
  const history = Array.isArray(s.winHistory) ? s.winHistory : [];
  if (!history.length || state.userHidWinHistory) {
    panel.classList.add('hidden');
    if (!history.length) panel.innerHTML = '';
    return;
  }
  panel.classList.remove('hidden');
  const winds = ['East', 'South', 'West', 'North'];
  const viewerSeat = (typeof s.viewerSeatIndex === 'number') ? s.viewerSeatIndex : null;
  const seats = s.seats || [];
  const items = history.map((entry, i) => {
    const num = i + 1;
    const headline = historyEntryLabel(entry, viewerSeat, seats);
    const tileName = entry.winningTile ? tileDisplayName(entry.winningTile) : '';
    const set = entry.settlement;
    let detail = '';
    if (set) {
      detail += `<div class="histFan">${set.fan} fan · base ${set.basePoints}</div>`;
      const deltas = set.deltas || {};
      const parts = winds
        .map((w) => {
          const k = w.toLowerCase();
          const v = deltas[k];
          if (typeof v !== 'number' || v === 0) return null;
          const sign = v > 0 ? '+' : '';
          const cls = v > 0 ? 'pos' : 'neg';
          return `<span class="hist${cls}">${w}: ${sign}${v}</span>`;
        })
        .filter(Boolean)
        .join(' · ');
      if (parts) detail += `<div class="histDeltas">${parts}</div>`;
    }
    return `<li class="histItem">
      <div class="histHead"><span class="histNum">#${num}</span> ${headline}</div>
      ${tileName ? `<div class="histTile">tile: <strong>${tileName}</strong></div>` : ''}
      ${detail}
    </li>`;
  }).join('');
  panel.innerHTML = `<h3>Win history (${history.length})</h3><ol class="histList">${items}</ol>`;
}

function renderActionButtons() {
  const container = $('actionButtons');
  container.innerHTML = '';
  const actions = ownLegalActions();
  // Don't show Discard buttons here — discards happen via tile click.
  // Don't show auto-draw button (auto-applied).
  const visible = actions.filter((a) => a.type !== 'discard' && a.type !== 'draw');
  visible.forEach((a) => {
    const btn = document.createElement('button');
    btn.className = 'action ' + (a.type === 'pass' ? 'passlike' : (a.type === 'win' ? 'win' : ''));
    btn.textContent = actionLabel(a);
    btn.addEventListener('click', () => sendAction(a));
    container.appendChild(btn);
  });
}

function actionLabel(a) {
  if (a.type === 'next_round') return 'Next round';
  if (a.type === 'draw') return 'Draw';
  if (a.type === 'pass') return 'Pass';
  if (a.type === 'chow') return chowLabel(a);
  if (a.type === 'pong') return 'Pong';
  if (a.type === 'kong') return 'Kong (' + a.kongType + ')';
  if (a.type === 'win') return 'Win!';
  return a.type;
}

// Compose a distinguishing label for a chow action like "Chow 3·4·_5_"
// (underscored = the claimed tile from the discard). Falls back to "Chow" if
// we cannot resolve the tiles for any reason.
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
  // All three tiles in a chow share the same suit (numerics 1-9). Sort by rank
  // so the label always reads e.g. "Chow 3·4·5" not "Chow 4·3·5".
  const trio = [t1, t2, claimed].slice().sort((x, y) => (x.rank || 0) - (y.rank || 0));
  const claimedKey = claimed.key;
  const parts = trio.map((t) => {
    const r = t.rank != null ? String(t.rank) : (t.key || '?');
    return t.key === claimedKey && t === trio.find((q) => q.key === claimedKey) ? `[${r}]` : r;
  });
  // Disambiguate further with the suit's first letter (b/c/d) so two chows
  // built on the same number but different suits are visibly different.
  const suit = (claimed.suit || '').charAt(0).toUpperCase();
  return suit ? `Chow ${parts.join('·')}${suit}` : `Chow ${parts.join('·')}`;
}

// =====================================================================
// Board rendering — layout mirrors the Win32 GUI (computeLayout + slots)
// =====================================================================

const canvas = () => $('boardCanvas');

const WINDS = ['East', 'South', 'West', 'North'];
const WIND_KANJI = { east: '東', south: '南', west: '西', north: '北' };

function fitCanvas() {
  const c = canvas();
  const rect = c.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(640, Math.floor(rect.width));
  const h = Math.max(480, Math.floor(rect.height));
  c.width = Math.floor(w * dpr);
  c.height = Math.floor(h * dpr);
  return { ctx: c.getContext('2d'), w, h, dpr };
}

// Compute table-level layout (mirrors Win32 computeLayout).
function computeLayout(w, h) {
  // Outer frame inset from canvas edge.
  const PAD = 12;
  const tableRect = { left: PAD, top: PAD, right: w - PAD, bottom: h - PAD };
  // Green felt inside the wood frame.
  const FRAME = 12;
  const feltRect = {
    left: tableRect.left + FRAME,
    top: tableRect.top + FRAME,
    right: tableRect.right - FRAME,
    bottom: tableRect.bottom - FRAME,
  };
  const cx = (feltRect.left + feltRect.right) / 2;
  const cy = (feltRect.top + feltRect.bottom) / 2;
  // Geometry scale factor: layout is designed around a 900-pixel-tall canvas.
  // For smaller canvases we shrink the discard wells, plaques, and seat offsets
  // so the hand still fits inside the felt.
  const designH = 900;
  const scale = Math.min(1, h / designH);
  return { tableRect, feltRect, cx, cy, scale };
}

// Per-slot geometry (slot 0 = bottom = viewer, 1 = right, 2 = top, 3 = left).
function slotForSeat(seatIndex, viewer) {
  return ((seatIndex - viewer) % 4 + 4) % 4;
}
// Rotation applied to opponent tiles so the face points at them
// (slot 0 = upright, 1 = right side, rotate -90°, 2 = top, rotate 180°, 3 = left, rotate +90°).
function angleForSlot(slot) {
  return [0, -Math.PI / 2, Math.PI, Math.PI / 2][slot];
}

// Geometry constants — match the Win32 numbers exactly so the layout feels the same.
// All "long axis" and offset values are scaled by `layout.scale` on small canvases.
const DESIGN = {
  DISCARD_LONG: 200,
  DISCARD_SHORT: 160,
  PLAQUE_LONG: 200,
  PLAQUE_SHORT: 44,
  CENTER_PLAQUE: 180,
  DISCARD_OFFSET: 95,     // center→discardWell inner edge
  PLAQUE_OFFSET: 132,     // feltEdge→plaque outer edge
  HAND_OFFSET: 50,        // feltEdge→handCenter
  DISCARD_CTR_OFFSET: 170,// center→discardCenter
  MELD_OFFSET: 280,       // center→meldStart along seat axis
  MELD_EDGE_OFFSET: 140,  // feltEdge→meld lane perpendicular (must clear hand row + small gap so melds are never tucked under hand on small screens)
  HAND_TILE_W: 42, HAND_TILE_H: 58,
  SIDE_TILE_W: 26, SIDE_TILE_H: 36,
  DISC_TILE_W: 28, DISC_TILE_H: 38,
  MELD_TILE_W: 28, MELD_TILE_H: 38,
};

function S(layout) { return layout.scale; }
function scaled(layout, v) { return v * layout.scale; }

function discardWellRect(layout, slot) {
  const { cx, cy } = layout;
  const W = scaled(layout, DESIGN.DISCARD_LONG);
  const T = scaled(layout, DESIGN.DISCARD_SHORT);
  const off = scaled(layout, DESIGN.DISCARD_OFFSET);
  switch (slot) {
    case 0: return { left: cx - W / 2, top: cy + off,     right: cx + W / 2, bottom: cy + off + T };
    case 2: return { left: cx - W / 2, top: cy - off - T, right: cx + W / 2, bottom: cy - off };
    case 1: return { left: cx + off,     top: cy - W / 2, right: cx + off + T, bottom: cy + W / 2 };
    case 3: return { left: cx - off - T, top: cy - W / 2, right: cx - off,     bottom: cy + W / 2 };
  }
}

function seatPlaqueRect(layout, slot) {
  const { feltRect, cx, cy } = layout;
  const L = scaled(layout, DESIGN.PLAQUE_LONG);
  const T = scaled(layout, DESIGN.PLAQUE_SHORT);
  const off = scaled(layout, DESIGN.PLAQUE_OFFSET);
  switch (slot) {
    case 0: return { left: cx - L / 2, top: feltRect.bottom - off, right: cx + L / 2, bottom: feltRect.bottom - off + T };
    case 2: return { left: cx - L / 2, top: feltRect.top + (off - T), right: cx + L / 2, bottom: feltRect.top + off };
    case 1: return { left: feltRect.right - off, top: cy - L / 2, right: feltRect.right - off + T, bottom: cy + L / 2 };
    case 3: return { left: feltRect.left + (off - T), top: cy - L / 2, right: feltRect.left + off, bottom: cy + L / 2 };
  }
}

// Each seat's hand-and-meld geometry.
function seatGeometry(layout, slot) {
  const { feltRect, cx, cy } = layout;
  const handEdge = scaled(layout, DESIGN.HAND_OFFSET);
  const discCtr  = scaled(layout, DESIGN.DISCARD_CTR_OFFSET);
  const meldAx   = scaled(layout, DESIGN.MELD_OFFSET);
  const meldEdge = scaled(layout, DESIGN.MELD_EDGE_OFFSET);
  switch (slot) {
    case 0: return { handCX: cx, handCY: feltRect.bottom - handEdge,           discardCX: cx, discardCY: cy + discCtr,  meldX: cx + meldAx, meldY: feltRect.bottom - meldEdge };
    case 2: return { handCX: cx, handCY: feltRect.top + handEdge,              discardCX: cx, discardCY: cy - discCtr,  meldX: cx - meldAx, meldY: feltRect.top + meldEdge };
    case 1: return { handCX: feltRect.right - handEdge, handCY: cy,            discardCX: cx + discCtr, discardCY: cy,  meldX: feltRect.right - meldEdge, meldY: cy + meldAx };
    case 3: return { handCX: feltRect.left + handEdge, handCY: cy,             discardCX: cx - discCtr, discardCY: cy,  meldX: feltRect.left + meldEdge, meldY: cy - meldAx };
  }
}

// ---------- Generic helpers ----------

function roundedRectPath(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.lineTo(x + w - r, y);
  ctx.quadraticCurveTo(x + w, y, x + w, y + r);
  ctx.lineTo(x + w, y + h - r);
  ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
  ctx.lineTo(x + r, y + h);
  ctx.quadraticCurveTo(x, y + h, x, y + h - r);
  ctx.lineTo(x, y + r);
  ctx.quadraticCurveTo(x, y, x + r, y);
  ctx.closePath();
}

function fillRoundRect(ctx, rect, radius, fillCss, strokeCss, strokeWidth) {
  const x = rect.left, y = rect.top, w = rect.right - rect.left, h = rect.bottom - rect.top;
  roundedRectPath(ctx, x, y, w, h, radius);
  ctx.fillStyle = fillCss;
  ctx.fill();
  if (strokeCss && strokeWidth > 0) {
    ctx.lineWidth = strokeWidth;
    ctx.strokeStyle = strokeCss;
    ctx.stroke();
  }
}

function drawText(ctx, text, x, y, opts = {}) {
  ctx.font = opts.font || '13px "Segoe UI", "Microsoft YaHei", sans-serif';
  ctx.fillStyle = opts.color || '#f6efd9';
  ctx.textAlign = opts.align || 'center';
  ctx.textBaseline = opts.baseline || 'middle';
  ctx.fillText(text, x, y);
}

// Wrap a draw call in a rotation around (cx, cy). Body draws as if upright.
function withRotation(ctx, cx, cy, angle, body) {
  ctx.save();
  ctx.translate(cx, cy);
  if (angle) ctx.rotate(angle);
  body();
  ctx.restore();
}

// Draw a tile centered on (cx, cy) with rotation. tile = null for face-down.
function drawTileCentered(ctx, tile, cx, cy, tw, th, angle, opts = {}) {
  withRotation(ctx, cx, cy, angle, () => {
    if (tile) TileRenderer.draw(ctx, tile, -tw / 2, -th / 2, tw, th, opts);
    else TileRenderer.drawBack(ctx, -tw / 2, -th / 2, tw, th);
  });
}

// ---------- Main draw ----------

function draw() {
  const s = state.snapshot;
  if (!s) return;
  const { ctx, w, h, dpr } = fitCanvas();
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  state.hitTargets = [];
  const layout = computeLayout(w, h);

  drawTableBackground(ctx, layout);
  drawDiscardWells(ctx, layout);
  drawCenterPlaque(ctx, layout, s);

  const viewer = state.seatIndex != null ? state.seatIndex : 0;
  // Draw discards + melds first (under hands and plaques).
  for (const player of s.players) {
    const slot = slotForSeat(player.seatIndex, viewer);
    drawDiscards(ctx, layout, s, player, slot);
    drawMelds(ctx, layout, s, player, slot);
    drawFlowers(ctx, layout, s, player, slot);
  }
  // Hands.
  for (const player of s.players) {
    const slot = slotForSeat(player.seatIndex, viewer);
    if (slot === 0) drawBottomHand(ctx, layout, s, player);
    else drawOpponentHand(ctx, layout, player, slot);
  }
  // Plaques on top so the wind badge is always visible.
  for (const player of s.players) {
    const slot = slotForSeat(player.seatIndex, viewer);
    drawSeatPlaque(ctx, layout, s, player, slot);
  }

  if (s.conclusion) drawConclusionBanner(ctx, layout, s);
}

// ---------- Table background ----------

function drawTableBackground(ctx, layout) {
  // Wood frame
  fillRoundRect(ctx, layout.tableRect, 26, '#78461f', '#3c2010', 2);
  // Inner highlight inset
  const hi = inflate(layout.tableRect, -4);
  fillRoundRect(ctx, hi, 22, '#965a30', '#5f371c', 1);
  // Felt
  fillRoundRect(ctx, layout.feltRect, 18, '#1f6e46', '#12462d', 2);
  // Inner ring (vignette)
  const ring = inflate(layout.feltRect, -28);
  ctx.strokeStyle = '#145a3c';
  ctx.lineWidth = 1;
  roundedRectPath(ctx, ring.left, ring.top, ring.right - ring.left, ring.bottom - ring.top, 16);
  ctx.stroke();
  // Diagonal corner lines toward center
  const { feltRect, cx, cy } = layout;
  ctx.strokeStyle = '#165033';
  ctx.beginPath();
  ctx.moveTo(feltRect.left + 80, feltRect.top + 80);    ctx.lineTo(cx - 90, cy - 90);
  ctx.moveTo(feltRect.right - 80, feltRect.top + 80);   ctx.lineTo(cx + 90, cy - 90);
  ctx.moveTo(feltRect.left + 80, feltRect.bottom - 80); ctx.lineTo(cx - 90, cy + 90);
  ctx.moveTo(feltRect.right - 80, feltRect.bottom - 80);ctx.lineTo(cx + 90, cy + 90);
  ctx.stroke();
}

function inflate(rect, d) {
  return { left: rect.left - d, top: rect.top - d, right: rect.right + d, bottom: rect.bottom + d };
}

// ---------- Discard wells ----------

function drawDiscardWells(ctx, layout) {
  for (let slot = 0; slot < 4; slot++) {
    const r = discardWellRect(layout, slot);
    fillRoundRect(ctx, r, 10, '#165840', '#508260', 1);
    const inset = inflate(r, -3);
    fillRoundRect(ctx, inset, 8, '#1a6040', '#326e4e', 1);
  }
}

// ---------- Center plaque ----------

function drawCenterPlaque(ctx, layout, s) {
  const { cx, cy } = layout;
  const half = scaled(layout, DESIGN.CENTER_PLAQUE) / 2;
  const plaque = { left: cx - half, top: cy - half, right: cx + half, bottom: cy + half };
  fillRoundRect(ctx, plaque, 18, '#3a2a12', '#e8c460', 3);
  fillRoundRect(ctx, inflate(plaque, -6), 14, '#18402c', '#a07840', 1);
  fillRoundRect(ctx, inflate(plaque, -10), 10, '#123224', '#3c5a46', 1);

  // Round label.
  drawText(ctx, `${WINDS[windIndex(s.prevailingWind)]} round`, cx, cy - half + scaled(layout, 18), {
    font: `bold ${Math.round(scaled(layout, 14))}px "Segoe UI", sans-serif`, color: '#e8c882',
  });
  // Big wind kanji.
  drawText(ctx, WIND_KANJI[s.prevailingWind] || '?', cx, cy - scaled(layout, 4), {
    font: `bold ${Math.round(scaled(layout, 76))}px "Microsoft YaHei", "PingFang SC", serif`, color: '#f0d282',
  });
  // Wall + turn line.
  drawText(ctx, `Wall ${s.liveWallCount}   Turn ${s.turnNumber}`, cx, cy + half - scaled(layout, 30), {
    font: `${Math.round(scaled(layout, 12))}px "Segoe UI", sans-serif`, color: '#e8dcaa',
  });
  // Phase / dealer line.
  const phaseShort = ({
    awaiting_draw: 'draw', awaiting_discard: 'discard',
    awaiting_claims: 'claims', finished: 'finished'
  })[s.phase] || s.phase;
  drawText(ctx, `${phaseShort}   Dealer ${WINDS[s.dealerSeat]}`, cx, cy + half - scaled(layout, 14), {
    font: `${Math.round(scaled(layout, 11))}px "Segoe UI", sans-serif`, color: '#d2c8a0',
  });
}

function windIndex(wind) {
  return { east: 0, south: 1, west: 2, north: 3 }[wind] ?? 0;
}

// ---------- Seat plaques ----------

function drawSeatPlaque(ctx, layout, s, player, slot) {
  const r = seatPlaqueRect(layout, slot);
  const isCurrent = s.currentTurn === player.seatIndex && s.phase !== 'finished';
  const isDealer = s.dealerSeat === player.seatIndex;
  const isYou = state.seatIndex === player.seatIndex;
  const fill = isCurrent ? '#405c40' : '#1e3024';
  const border = isCurrent ? '#ffc858' : '#78643c';
  fillRoundRect(ctx, r, 10, fill, border, isCurrent ? 2 : 1);

  // Wind badge.
  const horizontal = (slot === 0 || slot === 2);
  const badge = 32;
  const rectW = r.right - r.left, rectH = r.bottom - r.top;
  let badgeRect, textRect;
  if (slot === 0) {
    badgeRect = { left: r.left + 6, top: r.top + (rectH - badge) / 2, right: r.left + 6 + badge, bottom: r.top + (rectH - badge) / 2 + badge };
    textRect = { left: badgeRect.right + 6, top: r.top + 4, right: r.right - 6, bottom: r.bottom - 4, align: 'left' };
  } else if (slot === 2) {
    badgeRect = { left: r.right - 6 - badge, top: r.top + (rectH - badge) / 2, right: r.right - 6, bottom: r.top + (rectH - badge) / 2 + badge };
    textRect = { left: r.left + 6, top: r.top + 4, right: badgeRect.left - 6, bottom: r.bottom - 4, align: 'left' };
  } else if (slot === 1) {
    badgeRect = { left: r.left + (rectW - badge) / 2, top: r.top + 6, right: r.left + (rectW - badge) / 2 + badge, bottom: r.top + 6 + badge };
    textRect = { left: r.left + 2, top: badgeRect.bottom + 4, right: r.right - 2, bottom: r.bottom - 6, align: 'center' };
  } else {
    badgeRect = { left: r.left + (rectW - badge) / 2, top: r.bottom - 6 - badge, right: r.left + (rectW - badge) / 2 + badge, bottom: r.bottom - 6 };
    textRect = { left: r.left + 2, top: r.top + 6, right: r.right - 2, bottom: badgeRect.top - 4, align: 'center' };
  }
  const isEastWind = player.wind === 'east';
  fillRoundRect(ctx, badgeRect, 6, isEastWind ? '#c03838' : '#28406e', '#dcc88c', 1);
  drawText(ctx, WIND_KANJI[player.wind] || '?',
    (badgeRect.left + badgeRect.right) / 2, (badgeRect.top + badgeRect.bottom) / 2,
    { font: 'bold 22px "Microsoft YaHei", sans-serif', color: '#f5ebc8' });

  // Player name + score.
  // The "(you)" parenthetical must ONLY appear on the current viewer's
  // plaque, not on every human-controlled seat. When multiple humans share
  // a room, the older `controller === 'human' ? 'you' : 'AI'` labeled
  // every other human as "(you)" too, which was confusing. We now show:
  //   - "(you)" for the viewer's own seat
  //   - "(AI)" for AI seats
  //   - no parenthetical for other humans (just their display name)
  const role = isYou ? 'you' : (player.controller === 'human' ? '' : 'AI');
  const dealerMark = isDealer ? ' *' : '';
  const youMark = isYou ? ' ◀' : '';
  const line1 = role ? `${player.displayName} (${role})${youMark}` : `${player.displayName}${youMark}`;
  const line2 = `${player.score} pts${dealerMark}`;
  const tx = (textRect.left + textRect.right) / 2;
  const ty = (textRect.top + textRect.bottom) / 2;
  if (horizontal) {
    drawText(ctx, line1, textRect.left, ty - 8,
      { font: 'bold 12px "Segoe UI"', color: '#f4e8b6', align: 'left', baseline: 'middle' });
    drawText(ctx, line2, textRect.left, ty + 8,
      { font: '12px "Segoe UI"', color: isCurrent ? '#fff' : '#d8d0a0', align: 'left', baseline: 'middle' });
  } else {
    drawText(ctx, line1, tx, ty - 8,
      { font: 'bold 11px "Segoe UI"', color: '#f4e8b6' });
    drawText(ctx, line2, tx, ty + 8,
      { font: '11px "Segoe UI"', color: isCurrent ? '#fff' : '#d8d0a0' });
  }
}

// ---------- Hands ----------

function getDrawnTileId(s, seatIndex) {
  if (!s.lastDraw) return null;
  if (s.lastDraw.seatIndex !== seatIndex) return null;
  if (s.phase !== 'awaiting_discard') return null;
  if (s.currentTurn !== seatIndex) return null;
  return s.lastDraw.tile.id;
}

function drawBottomHand(ctx, layout, s, player) {
  const tiles = (player.concealedTiles || []).slice();
  if (!tiles.length) return;
  const geom = seatGeometry(layout, 0);
  const drawnId = getDrawnTileId(s, player.seatIndex);
  let drawnTile = null;
  let rest = tiles;
  if (drawnId) {
    drawnTile = tiles.find((t) => t.id === drawnId) || null;
    rest = tiles.filter((t) => t.id !== drawnId);
  }
  rest = sortTilesClient(rest);
  const ordered = drawnTile ? rest.concat([drawnTile]) : rest;

  const tw = scaled(layout, DESIGN.HAND_TILE_W);
  const th = scaled(layout, DESIGN.HAND_TILE_H);
  const gap = 2;
  const extraGap = scaled(layout, 10);
  const n = ordered.length;
  const total = n * tw + (n - 1) * gap + (drawnTile ? extraGap : 0);
  let x = geom.handCX - total / 2;
  const y = geom.handCY - th / 2;
  for (let i = 0; i < ordered.length; i++) {
    const t = ordered[i];
    const highlight = t === drawnTile ? 2 : (t.id === state.hoverTileId ? 1 : 0);
    TileRenderer.draw(ctx, t, x, y, tw, th, { highlight });
    state.hitTargets.push({ tileId: t.id, kind: 'hand', x, y, w: tw, h: th });
    x += tw + gap;
    if (drawnTile && i === ordered.length - 2) x += extraGap;
  }
}

function drawOpponentHand(ctx, layout, player, slot) {
  const count = player.concealedCount || 0;
  if (!count) return;
  const geom = seatGeometry(layout, slot);
  const tw = scaled(layout, DESIGN.SIDE_TILE_W);
  const th = scaled(layout, DESIGN.SIDE_TILE_H);
  const gap = 2;
  const angle = angleForSlot(slot);
  const total = count * tw + (count - 1) * gap;
  const tiles = player.concealedTiles && player.concealedTiles.length === count ? player.concealedTiles : null;
  for (let i = 0; i < count; i++) {
    const lx = -total / 2 + tw / 2 + i * (tw + gap);
    const ly = 0;
    const wx = geom.handCX + Math.cos(angle) * lx + Math.sin(angle) * ly;
    const wy = geom.handCY - Math.sin(angle) * lx + Math.cos(angle) * ly;
    if (tiles) drawTileCentered(ctx, tiles[i], wx, wy, tw, th, angle);
    else withRotation(ctx, wx, wy, angle, () => TileRenderer.drawBack(ctx, -tw / 2, -th / 2, tw, th));
  }
}

// ---------- Melds ----------

function drawMelds(ctx, layout, s, player, slot) {
  if (!player.melds || !player.melds.length) return;
  const geom = seatGeometry(layout, slot);
  const angle = angleForSlot(slot);
  const tw = scaled(layout, DESIGN.MELD_TILE_W);
  const th = scaled(layout, DESIGN.MELD_TILE_H);
  const gap = 1;
  const meldGap = scaled(layout, 8);

  let cursor = 0;
  for (const meld of player.melds) {
    for (const t of meld.tiles) {
      const localOffset = cursor + tw / 2;
      cursor += tw + gap;
      const baseX = geom.meldX, baseY = geom.meldY;
      let dirX = 0, dirY = 0;
      switch (slot) {
        case 0: dirX = -1; break;
        case 2: dirX =  1; break;
        case 1: dirY = -1; break;
        case 3: dirY =  1; break;
      }
      const wx = baseX + dirX * localOffset;
      const wy = baseY + dirY * localOffset;
      const showTile = meld.concealed ? null : t;
      drawTileCentered(ctx, showTile, wx, wy, tw, th, angle);
    }
    cursor += meldGap;
  }
}

// ---------- Flowers ----------

function drawFlowers(ctx, layout, s, player, slot) {
  if (!player.flowers || !player.flowers.length) return;
  const { feltRect } = layout;
  const tw = scaled(layout, DESIGN.SIDE_TILE_W);
  const th = scaled(layout, DESIGN.SIDE_TILE_H);
  const gap = 2;
  const angle = angleForSlot(slot);
  const cornerInset = scaled(layout, 24);
  const laneInset = scaled(layout, 56);
  let baseX = 0, baseY = 0, dirX = 0, dirY = 0;
  switch (slot) {
    case 0: baseX = feltRect.left + cornerInset;  baseY = feltRect.bottom - laneInset; dirX = 1; break;
    case 2: baseX = feltRect.right - cornerInset; baseY = feltRect.top + laneInset;    dirX = -1; break;
    case 1: baseX = feltRect.right - laneInset;   baseY = feltRect.top + cornerInset;  dirY = 1; break;
    case 3: baseX = feltRect.left + laneInset;    baseY = feltRect.bottom - cornerInset; dirY = -1; break;
  }
  for (let i = 0; i < player.flowers.length; i++) {
    const offset = i * (tw + gap) + tw / 2;
    const wx = baseX + dirX * offset;
    const wy = baseY + dirY * offset;
    drawTileCentered(ctx, player.flowers[i], wx, wy, tw, th, angle);
  }
}

// ---------- Discards ----------

function drawDiscards(ctx, layout, s, player, slot) {
  if (!player.discards || !player.discards.length) return;
  const geom = seatGeometry(layout, slot);
  const angle = angleForSlot(slot);
  const tw = scaled(layout, DESIGN.DISC_TILE_W);
  const th = scaled(layout, DESIGN.DISC_TILE_H);
  const gap = 2, columns = 6;
  const rows = Math.ceil(player.discards.length / columns);
  const gridW = columns * tw + (columns - 1) * gap;
  const gridH = Math.max(1, rows) * th + Math.max(0, rows - 1) * gap;
  const lastDiscardId = s.lastDiscard && s.lastDiscard.bySeat === player.seatIndex ? s.lastDiscard.tile.id : null;
  const ca = Math.cos(angle), sa = Math.sin(angle);
  for (let i = 0; i < player.discards.length; i++) {
    const row = Math.floor(i / columns);
    const col = i % columns;
    const lx = -gridW / 2 + col * (tw + gap) + tw / 2;
    const ly = -gridH / 2 + row * (th + gap) + th / 2;
    const wx = geom.discardCX + lx * ca + ly * sa;
    const wy = geom.discardCY + (-lx * sa) + ly * ca;
    const highlight = lastDiscardId && player.discards[i].id === lastDiscardId ? 1 : 0;
    drawTileCentered(ctx, player.discards[i], wx, wy, tw, th, angle, { highlight });
  }
}

// ---------- Conclusion banner ----------

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

function drawConclusionBanner(ctx, layout, s) {
  const conclusion = s.conclusion;
  if (!conclusion) return;
  const { cx, cy } = layout;
  const hasWinTile = !!conclusion.winningTile;
  const w = 580;
  // Reserve extra vertical space when we have a winning tile to display.
  const h = hasWinTile ? 280 : 220;
  const banner = { left: cx - w / 2, top: cy - h / 2, right: cx + w / 2, bottom: cy + h / 2 };
  // shadow
  ctx.save();
  ctx.fillStyle = 'rgba(0,0,0,0.6)';
  roundedRectPath(ctx, banner.left + 4, banner.top + 4, w, h, 14);
  ctx.fill();
  ctx.restore();
  fillRoundRect(ctx, banner, 14, '#28180e', '#f0c870', 3);
  fillRoundRect(ctx, inflate(banner, -6), 10, '#38241a', '#a07840', 1);

  drawText(ctx, conclusion.message || 'Round complete',
    cx, banner.top + 26, { font: 'bold 18px "Segoe UI", sans-serif', color: '#f4dc96' });

  // Win source label + winning tile (the highlight everyone cares about).
  let bodyTop = banner.top + 50;
  if (hasWinTile) {
    const srcText = winSourceLabel(s);
    if (srcText) {
      drawText(ctx, srcText,
        cx, bodyTop + 8, { font: 'bold 14px "Segoe UI"', color: '#ffe9a8' });
    }
    // Render the winning tile, large enough to read at a glance.
    const tw = 54, th = 72;
    const tileCx = cx;
    const tileCy = bodyTop + 28 + th / 2;
    // Subtle gold glow behind the tile so it pops against the banner.
    ctx.save();
    ctx.shadowColor = '#ffd86b';
    ctx.shadowBlur = 18;
    drawTileCentered(ctx, conclusion.winningTile, tileCx, tileCy, tw, th, 0, { highlight: 2 });
    ctx.restore();
    bodyTop = tileCy + th / 2 + 8;
  }

  if (conclusion.settlement) {
    const set = conclusion.settlement;
    const minMsg = set.eligible ? '' : '  (below minimum)';
    drawText(ctx, `Fan: ${set.fan}  (min ${set.minFan})${minMsg}`,
      cx, bodyTop + 12, { font: '14px "Segoe UI"', color: '#e8dcb4' });
    let features = (set.includedFeatures || []).map((f) => f.name).join(', ') || '(no fan features)';
    drawText(ctx, features, cx, bodyTop + 36, { font: '12px "Segoe UI"', color: '#dcd0a8' });
    let y = bodyTop + 60;
    for (const line of set.paymentLines || []) {
      drawText(ctx, `${line.from} pays ${line.to}: ${line.points}`,
        cx, y, { font: '13px "Segoe UI"', color: '#f0e4b8' });
      y += 18;
      if (y > banner.bottom - 8) break;
    }
  }
}

// ---------- Sort (mirrors server) ----------

function sortTilesClient(tiles) {
  return tiles.slice().sort((a, b) => tileSortKey(a) - tileSortKey(b));
}
function tileSortKey(t) {
  const catBase = { suit: 0, wind: 300, dragon: 400, flower: 500, season: 600 }[t.category] || 999;
  let suitOffset = 0;
  if (t.category === 'suit') suitOffset = { dots: 0, bamboo: 100, characters: 200 }[t.suit] || 0;
  const rank = t.rank
    || (t.wind ? ({ east: 1, south: 2, west: 3, north: 4 }[t.wind]) : 0)
    || (t.dragon ? ({ red: 1, green: 2, white: 3 }[t.dragon]) : 0);
  return catBase + suitOffset + rank;
}

// ---------- Tile click → discard ----------
function setupCanvasInput() {
  const c = canvas();
  // Convert browser event coords (CSS pixels relative to viewport) into the
  // canvas's *design* coordinate system that state.hitTargets uses. The two
  // can differ when the canvas's CSS width is smaller than its design width
  // (e.g., 390 CSS px on iPhone 12 vs design width clamped to >= 640) —
  // without this rescale, tile taps land off-target on phones. See fitCanvas.
  const eventToDesign = (clientX, clientY) => {
    const rect = c.getBoundingClientRect();
    const designW = Math.max(640, Math.floor(rect.width));
    const designH = Math.max(480, Math.floor(rect.height));
    const sx = designW / rect.width;
    const sy = designH / rect.height;
    return { px: (clientX - rect.left) * sx, py: (clientY - rect.top) * sy };
  };
  c.addEventListener('mousemove', (e) => {
    const { px, py } = eventToDesign(e.clientX, e.clientY);
    const target = hitTest(px, py);
    const newHover = target ? target.tileId : null;
    if (newHover !== state.hoverTileId) {
      state.hoverTileId = newHover;
      draw();
    }
  });
  c.addEventListener('mouseleave', () => {
    if (state.hoverTileId) { state.hoverTileId = null; draw(); }
  });
  const tryDiscardAt = (px, py) => {
    const target = hitTest(px, py);
    if (!target || target.kind !== 'hand') return false;
    const actions = ownLegalActions();
    const discard = actions.find((a) => a.type === 'discard' && a.tileId === target.tileId);
    if (discard) { sendAction(discard); return true; }
    return false;
  };
  c.addEventListener('click', (e) => {
    const { px, py } = eventToDesign(e.clientX, e.clientY);
    tryDiscardAt(px, py);
  });
  // Touch handler for phones/tablets. We use touchstart for immediate response
  // and call preventDefault so the browser does NOT also synthesize a click
  // (which would discard twice). We also clear any sticky hover state since
  // touch devices don't have a "mouse left the area" event.
  c.addEventListener('touchstart', (e) => {
    if (e.touches.length !== 1) return;        // ignore multi-touch / pinch
    const t = e.touches[0];
    const { px, py } = eventToDesign(t.clientX, t.clientY);
    const acted = tryDiscardAt(px, py);
    if (acted) e.preventDefault();
    if (state.hoverTileId) { state.hoverTileId = null; draw(); }
  }, { passive: false });
}

function hitTest(px, py) {
  for (let i = state.hitTargets.length - 1; i >= 0; i--) {
    const t = state.hitTargets[i];
    if (px >= t.x && px <= t.x + t.w && py >= t.y && py <= t.y + t.h) return t;
  }
  return null;
}

// ---------- Init ----------
window.addEventListener('load', () => {
  setupLobby();
  setupCanvasInput();
  window.addEventListener('resize', () => { if (state.snapshot) draw(); });
  // Phones fire both `orientationchange` and `resize`; debounce a redraw on
  // either path. Some browsers don't fire resize on rotation, so we listen
  // explicitly. Use a small timeout to wait for layout to settle.
  const redrawSoon = () => setTimeout(() => { if (state.snapshot) draw(); }, 150);
  window.addEventListener('orientationchange', redrawSoon);
  if (window.matchMedia) {
    try {
      window.matchMedia('(orientation: portrait)').addEventListener('change', redrawSoon);
    } catch (_) { /* old Safari: no addEventListener on MediaQueryList */ }
  }
});
