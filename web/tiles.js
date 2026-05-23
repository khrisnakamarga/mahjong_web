// Canvas tile renderer ported from the Win32 GDI version.
// Public API:
//   TileRenderer.draw(ctx, tile, x, y, w, h, opts)
//   TileRenderer.drawBack(ctx, x, y, w, h)
//   TileRenderer.tileFace(tile)  // string label for fallback
// Tile shape mirrors the JSON snapshot: { key, category, suit, rank, wind, dragon, name, id }
//
// Visual style guidelines (matches the reference image the user provided):
//   - Dots are drawn as "donut" rings with four cardinal tick marks and a
//     small center dot, like authentic Hong Kong Mahjong circles.
//   - Bamboos are drawn as upright pill-shaped stalks divided into 5
//     visible segments by thin transverse lines. The 1-bamboo is a small
//     sparrow (red body + green wing). The 8-bamboo arranges 8 stalks in
//     an "M over W" angled fan pattern.
//   - Characters use bold Chinese numerals on top and 萬 in red on bottom.
//   - **No red 5s anywhere**: the 5-dots, 5-bamboo, and 5-characters all
//     use the same non-red coloring as their non-5 neighbors. The small
//     top-left index label is also rendered in a neutral grey so the
//     overall tile never contains a prominent red "5".
const TileRenderer = (() => {
  const COLOR = {
    face: '#f4ecd2',
    faceHover: '#fde58a',
    faceDrawn: '#ffe7a0',
    edge: '#9b8a5f',
    edgeDark: '#6e5d33',
    red: '#b81d24',
    green: '#1e6a32',
    greenDark: '#0d4a23',
    teal: '#0e5a48',
    blue: '#1c3c96',
    gold: '#c89a3a',
    ink: '#1a1a1a',
    black: '#1a1a1a',
    indexGrey: '#766a4a',
  };

  function roundedRect(ctx, x, y, w, h, r) {
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

  function drawFrame(ctx, x, y, w, h, highlight) {
    const r = Math.max(4, Math.min(8, Math.floor(w * 0.12)));
    // shadow
    ctx.fillStyle = 'rgba(0,0,0,0.35)';
    roundedRect(ctx, x + 1, y + 3, w, h, r);
    ctx.fill();
    // face
    let face = COLOR.face;
    if (highlight === 1) face = COLOR.faceHover;
    if (highlight === 2) face = COLOR.faceDrawn;
    ctx.fillStyle = face;
    roundedRect(ctx, x, y, w, h, r);
    ctx.fill();
    // top highlight
    const grad = ctx.createLinearGradient(x, y, x, y + h * 0.5);
    grad.addColorStop(0, 'rgba(255,255,255,0.45)');
    grad.addColorStop(1, 'rgba(255,255,255,0)');
    ctx.fillStyle = grad;
    roundedRect(ctx, x + 1, y + 1, w - 2, h * 0.45, r);
    ctx.fill();
    // edge
    ctx.strokeStyle = COLOR.edge;
    ctx.lineWidth = 1.5;
    roundedRect(ctx, x + 0.5, y + 0.5, w - 1, h - 1, r);
    ctx.stroke();
    // bottom shadow stripe
    ctx.fillStyle = 'rgba(0,0,0,0.15)';
    ctx.fillRect(x + 2, y + h - Math.max(3, h * 0.08), w - 4, Math.max(2, h * 0.05));
  }

  const FLOWER_RANK_BY_KEY = { 'flower-plum': 1, 'flower-orchid': 2, 'flower-chrysanthemum': 3, 'flower-bamboo': 4 };
  const SEASON_RANK_BY_KEY = { 'season-spring': 1, 'season-summer': 2, 'season-autumn': 3, 'season-winter': 4 };
  function flowerSeasonRank(tile) {
    if (tile.rank) return tile.rank;
    return (tile.category === 'flower' ? FLOWER_RANK_BY_KEY : SEASON_RANK_BY_KEY)[tile.key] || 0;
  }

  function indexLabel(tile) {
    if (!tile) return '';
    if (tile.category === 'suit' && tile.rank) return String(tile.rank);
    if (tile.category === 'wind') {
      return { east: 'E', south: 'S', west: 'W', north: 'N' }[tile.wind] || '';
    }
    if (tile.category === 'dragon') {
      return { red: 'C', green: 'F', white: 'P' }[tile.dragon] || '';
    }
    if (tile.category === 'flower' || tile.category === 'season') {
      const r = flowerSeasonRank(tile);
      return r ? String(r) : '';
    }
    return '';
  }

  function drawSmallIndex(ctx, tile, x, y, w) {
    const label = indexLabel(tile);
    if (!label) return;
    ctx.save();
    // Neutral grey -- never red, so 5-tiles never have a red "5" badge.
    ctx.fillStyle = COLOR.indexGrey;
    ctx.font = `bold ${Math.max(9, Math.floor(w * 0.20))}px "Segoe UI", sans-serif`;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'top';
    ctx.fillText(label, x + 3, y + 2);
    ctx.restore();
  }

  // ---------- Dots (circles) ----------
  // Each dot is a donut ring + tiny center bead + four short cardinal ticks.
  // Matches the authentic Hong Kong Mahjong circles seen in the reference.
  function drawDot(ctx, cx, cy, r, color) {
    ctx.save();
    // outer ring
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, Math.PI * 2);
    ctx.fill();
    // inner face cutout
    ctx.fillStyle = COLOR.face;
    ctx.beginPath();
    ctx.arc(cx, cy, r * 0.55, 0, Math.PI * 2);
    ctx.fill();
    // tiny center bead
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(cx, cy, r * 0.20, 0, Math.PI * 2);
    ctx.fill();
    // 4 cardinal tick marks bridging the inner ring to the outer ring.
    const tickW = Math.max(1, r * 0.22);
    const tickL = r * 0.30;
    // left
    ctx.fillRect(cx - r * 0.95, cy - tickW / 2, tickL, tickW);
    // right
    ctx.fillRect(cx + r * 0.65, cy - tickW / 2, tickL, tickW);
    // top
    ctx.fillRect(cx - tickW / 2, cy - r * 0.95, tickW, tickL);
    // bottom
    ctx.fillRect(cx - tickW / 2, cy + r * 0.65, tickW, tickL);
    ctx.restore();
  }

  function drawOrnateOne(ctx, cx, cy, r) {
    // 1-circle: a large decorative medallion. Concentric rings + 8 petals.
    ctx.save();
    // outer teal ring
    ctx.fillStyle = COLOR.teal;
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, Math.PI * 2);
    ctx.fill();
    // face cutout
    ctx.fillStyle = COLOR.face;
    ctx.beginPath();
    ctx.arc(cx, cy, r * 0.84, 0, Math.PI * 2);
    ctx.fill();
    // inner teal ring
    ctx.fillStyle = COLOR.teal;
    ctx.beginPath();
    ctx.arc(cx, cy, r * 0.70, 0, Math.PI * 2);
    ctx.fill();
    ctx.fillStyle = COLOR.face;
    ctx.beginPath();
    ctx.arc(cx, cy, r * 0.50, 0, Math.PI * 2);
    ctx.fill();
    // 8 petals around center
    ctx.fillStyle = COLOR.teal;
    for (let i = 0; i < 8; i++) {
      const a = (i / 8) * Math.PI * 2;
      const px = cx + Math.cos(a) * r * 0.42;
      const py = cy + Math.sin(a) * r * 0.42;
      ctx.beginPath();
      ctx.ellipse(px, py, r * 0.11, r * 0.16, a + Math.PI / 2, 0, Math.PI * 2);
      ctx.fill();
    }
    // small center bead -- never red (mirrors the no-red-5 rule); use teal
    ctx.fillStyle = COLOR.teal;
    ctx.beginPath();
    ctx.arc(cx, cy, r * 0.17, 0, Math.PI * 2);
    ctx.fill();
    ctx.restore();
  }

  function dotPositions(rank) {
    switch (rank) {
      case 1: return [[0.50, 0.50]];
      case 2: return [[0.50, 0.28], [0.50, 0.72]];
      case 3: return [[0.27, 0.22], [0.50, 0.50], [0.73, 0.78]];
      case 4: return [[0.30, 0.28], [0.70, 0.28], [0.30, 0.72], [0.70, 0.72]];
      case 5: return [[0.28, 0.25], [0.72, 0.25], [0.50, 0.50], [0.28, 0.75], [0.72, 0.75]];
      case 6: return [[0.26, 0.28], [0.50, 0.28], [0.74, 0.28], [0.26, 0.72], [0.50, 0.72], [0.74, 0.72]];
      case 7: return [[0.26, 0.18], [0.50, 0.18], [0.74, 0.18], [0.26, 0.55], [0.50, 0.55], [0.74, 0.55], [0.50, 0.85]];
      case 8: return [[0.30, 0.18], [0.70, 0.18], [0.30, 0.40], [0.70, 0.40], [0.30, 0.60], [0.70, 0.60], [0.30, 0.82], [0.70, 0.82]];
      case 9: return [[0.25, 0.20], [0.50, 0.20], [0.75, 0.20], [0.25, 0.50], [0.50, 0.50], [0.75, 0.50], [0.25, 0.80], [0.50, 0.80], [0.75, 0.80]];
      default: return [];
    }
  }

  function dotColor(rank, index) {
    // Authentic Hong Kong Mahjong dot coloring, adjusted so 5-dots has NO
    // red component (per user requirement). Other ranks keep their classic
    // red/black mixes.
    if (rank === 1) return COLOR.teal;     // (medallion uses its own draw)
    if (rank === 2) return COLOR.black;
    if (rank === 3) return index === 1 ? COLOR.red : COLOR.black; // center is red
    if (rank === 4) return COLOR.black;
    if (rank === 5) return COLOR.black;    // no red 5
    if (rank === 6) return index < 3 ? COLOR.red : COLOR.black;   // top row red
    if (rank === 7) return index < 3 ? COLOR.red : COLOR.black;   // top 3 red
    if (rank === 8) return COLOR.black;
    if (rank === 9) {
      // top row black, middle row red, bottom row black
      if (index < 3) return COLOR.black;
      if (index < 6) return COLOR.red;
      return COLOR.black;
    }
    return COLOR.black;
  }

  function drawDots(ctx, tile, x, y, w, h) {
    if (tile.rank === 1) {
      drawOrnateOne(ctx, x + w / 2, y + h / 2, Math.min(w, h) * 0.42);
      return;
    }
    const positions = dotPositions(tile.rank);
    if (!positions.length) return;
    // Smaller dots for higher-density layouts so they don't crowd.
    const dense = tile.rank >= 6;
    const radius = Math.max(3, Math.min(w, h) * (dense ? 0.085 : 0.105));
    positions.forEach(([fx, fy], i) => {
      drawDot(ctx, x + fx * w, y + fy * h, radius, dotColor(tile.rank, i));
    });
  }

  // ---------- Bamboo (sticks) ----------
  // Each stalk is a pill divided into 5 visible segments by thin lines,
  // mirroring the joint-and-internode look of the reference image.
  function drawStalk(ctx, cx, cy, len, color, segments = 5) {
    const sw = Math.max(2, len * 0.20);
    const x = cx - sw / 2;
    const y = cy - len / 2;
    ctx.save();
    ctx.fillStyle = color;
    if (typeof ctx.roundRect === 'function') {
      ctx.beginPath();
      ctx.roundRect(x, y, sw, len, sw * 0.45);
      ctx.fill();
    } else {
      // Fallback for older canvases: rect + end-caps.
      ctx.fillRect(x, y, sw, len);
      ctx.beginPath();
      ctx.arc(cx, y, sw / 2, Math.PI, 0);
      ctx.fill();
      ctx.beginPath();
      ctx.arc(cx, y + len, sw / 2, 0, Math.PI);
      ctx.fill();
    }
    // Segment-divider lines (background-coloured thin lines).
    ctx.strokeStyle = COLOR.face;
    ctx.lineWidth = Math.max(0.8, sw * 0.18);
    for (let i = 1; i < segments; i++) {
      const sy = y + (len / segments) * i;
      ctx.beginPath();
      ctx.moveTo(x + sw * 0.10, sy);
      ctx.lineTo(x + sw * 0.90, sy);
      ctx.stroke();
    }
    ctx.restore();
  }

  function drawStalkAt(ctx, cx, cy, len, color, angleDeg) {
    if (!angleDeg) { drawStalk(ctx, cx, cy, len, color); return; }
    ctx.save();
    ctx.translate(cx, cy);
    ctx.rotate(angleDeg * Math.PI / 180);
    drawStalk(ctx, 0, 0, len, color);
    ctx.restore();
  }

  function drawBird(ctx, x, y, w, h) {
    // 1-bamboo: small sparrow. Body+head in red, wing+tail accents in green.
    const s = Math.min(w, h);
    const cx = x + w * 0.48;
    const cy = y + h * 0.52;
    ctx.save();
    // tail feathers (green, fanning down-left)
    ctx.strokeStyle = COLOR.green;
    ctx.lineWidth = Math.max(1.5, s * 0.035);
    ctx.lineCap = 'round';
    for (let i = -1; i <= 1; i++) {
      ctx.beginPath();
      ctx.moveTo(cx - s * 0.05, cy + s * 0.18);
      ctx.quadraticCurveTo(cx - s * 0.18, cy + s * 0.30 + i * s * 0.03, cx - s * 0.30, cy + s * 0.40 + i * s * 0.05);
      ctx.stroke();
    }
    // body (red ellipse)
    ctx.fillStyle = COLOR.red;
    ctx.beginPath();
    ctx.ellipse(cx, cy + s * 0.10, s * 0.22, s * 0.16, -0.18, 0, Math.PI * 2);
    ctx.fill();
    // wing (green over body)
    ctx.fillStyle = COLOR.green;
    ctx.beginPath();
    ctx.ellipse(cx - s * 0.05, cy + s * 0.08, s * 0.18, s * 0.09, -0.35, 0, Math.PI * 2);
    ctx.fill();
    // small green feather tips on wing
    ctx.strokeStyle = COLOR.greenDark;
    ctx.lineWidth = Math.max(0.8, s * 0.012);
    for (let i = 0; i < 3; i++) {
      ctx.beginPath();
      ctx.moveTo(cx - s * 0.10 + i * s * 0.05, cy + s * 0.13);
      ctx.lineTo(cx - s * 0.05 + i * s * 0.05, cy + s * 0.20);
      ctx.stroke();
    }
    // head (red disc, up-right of body)
    ctx.fillStyle = COLOR.red;
    ctx.beginPath();
    ctx.arc(cx + s * 0.16, cy - s * 0.14, s * 0.13, 0, Math.PI * 2);
    ctx.fill();
    // beak (gold triangle pointing right)
    ctx.fillStyle = COLOR.gold;
    ctx.beginPath();
    ctx.moveTo(cx + s * 0.28, cy - s * 0.16);
    ctx.lineTo(cx + s * 0.40, cy - s * 0.10);
    ctx.lineTo(cx + s * 0.28, cy - s * 0.06);
    ctx.closePath();
    ctx.fill();
    // eye
    ctx.fillStyle = COLOR.black;
    ctx.beginPath();
    ctx.arc(cx + s * 0.20, cy - s * 0.17, Math.max(1, s * 0.020), 0, Math.PI * 2);
    ctx.fill();
    // tiny green legs
    ctx.strokeStyle = COLOR.green;
    ctx.lineWidth = Math.max(0.8, s * 0.014);
    ctx.beginPath();
    ctx.moveTo(cx - s * 0.02, cy + s * 0.24);
    ctx.lineTo(cx - s * 0.04, cy + s * 0.34);
    ctx.moveTo(cx + s * 0.05, cy + s * 0.24);
    ctx.lineTo(cx + s * 0.03, cy + s * 0.34);
    ctx.stroke();
    ctx.restore();
  }

  function bambooLayout(rank) {
    // Each entry is a row, each row is an array of column fractions in 0..1.
    // Stalks are drawn upright for these layouts.
    switch (rank) {
      case 2: return [[0.50], [0.50]];
      case 3: return [[0.50], [0.30, 0.70]];                       // triangle: 1 top, 2 bottom
      case 4: return [[0.30, 0.70], [0.30, 0.70]];                 // 2x2
      case 5: return [[0.30, 0.70], [0.50], [0.30, 0.70]];         // X pattern (corners + center)
      case 6: return [[0.25, 0.50, 0.75], [0.25, 0.50, 0.75]];     // 3x2
      case 7: return [[0.50], [0.25, 0.50, 0.75], [0.25, 0.50, 0.75]];  // 1 on top + 6 below
      case 9: return [[0.25, 0.50, 0.75], [0.25, 0.50, 0.75], [0.25, 0.50, 0.75]];  // 3x3
      default: return null;
    }
  }

  function bambooColor(rank, rowIdx) {
    // Color overrides matching the reference image. Default is green.
    // **No red 5s**: 5-bamboo stays all green even though some traditional
    // sets put a red center stick.
    if (rank === 7 && rowIdx === 0) return COLOR.red;  // top centre stick of 7 is red
    return COLOR.green;
  }

  function drawBamboo8(ctx, x, y, w, h) {
    // 8-bamboo arranges 8 stalks in a top-M / bottom-W fan pattern.
    // Top row: 4 stalks tilt inward toward the centre (bottoms converge).
    // Bottom row: 4 stalks tilt outward from the centre (tops converge).
    const stickLen = h * 0.36;
    const yTop = y + h * 0.30;
    const yBot = y + h * 0.70;
    const xs = [0.18, 0.40, 0.60, 0.82];
    const topAngles = [22, 8, -8, -22];   // M: outer tilt out, inner tilt in
    const botAngles = [-22, -8, 8, 22];   // W: outer tilt out, inner tilt in (flipped)
    for (let i = 0; i < 4; i++) {
      drawStalkAt(ctx, x + xs[i] * w, yTop, stickLen, COLOR.green, topAngles[i]);
      drawStalkAt(ctx, x + xs[i] * w, yBot, stickLen, COLOR.green, botAngles[i]);
    }
  }

  function drawBamboo(ctx, tile, x, y, w, h) {
    if (tile.rank === 1) { drawBird(ctx, x, y, w, h); return; }
    if (tile.rank === 8) { drawBamboo8(ctx, x, y, w, h); return; }
    const rows = bambooLayout(tile.rank);
    if (!rows) return;
    const rowCount = rows.length;
    const innerH = h * 0.82;
    const yStart = y + h * 0.09;
    const rowHeight = innerH / rowCount;
    const maxCols = Math.max(...rows.map((r) => r.length));
    // Stick length scales with row height; clamp by column width so dense
    // 3-col rows don't get oversized stalks.
    const colWidth = w / maxCols;
    const stickLen = Math.min(rowHeight * 0.88, colWidth * 1.4);
    rows.forEach((row, rIdx) => {
      const fy = yStart + rowHeight * (rIdx + 0.5);
      row.forEach((fx) => {
        drawStalk(ctx, x + fx * w, fy, stickLen, bambooColor(tile.rank, rIdx));
      });
    });
  }

  function chineseNumeral(n) {
    // Traditional: 5 = 伍 (formal), 7 = 七, etc.
    return ['', '一', '二', '三', '四', '伍', '六', '七', '八', '九'][n] || String(n);
  }

  function drawCharacters(ctx, tile, x, y, w, h) {
    // Top: rank numeral in black (never red, including for rank 5).
    // Bottom: 萬 always in red.
    ctx.save();
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillStyle = COLOR.black;
    ctx.font = `bold ${Math.floor(h * 0.36)}px "PingFang SC", "Microsoft YaHei", "SimSun", serif`;
    ctx.fillText(chineseNumeral(tile.rank), x + w / 2, y + h * 0.30);
    ctx.fillStyle = COLOR.red;
    ctx.font = `bold ${Math.floor(h * 0.42)}px "PingFang SC", "Microsoft YaHei", "SimSun", serif`;
    ctx.fillText('萬', x + w / 2, y + h * 0.68);
    ctx.restore();
  }

  function drawWind(ctx, tile, x, y, w, h) {
    const kanji = { east: '東', south: '南', west: '西', north: '北' }[tile.wind] || '';
    const color = tile.wind === 'east' ? COLOR.red : COLOR.blue;
    ctx.save();
    ctx.fillStyle = color;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.font = `bold ${Math.floor(h * 0.62)}px "PingFang SC", "Microsoft YaHei", "SimSun", serif`;
    ctx.fillText(kanji, x + w / 2, y + h * 0.55);
    ctx.restore();
  }

  function drawDragon(ctx, tile, x, y, w, h) {
    ctx.save();
    if (tile.dragon === 'white') {
      ctx.strokeStyle = COLOR.blue;
      ctx.lineWidth = Math.max(3, w * 0.07);
      const pad = w * 0.18;
      ctx.strokeRect(x + pad, y + pad, w - 2 * pad, h - 2 * pad);
    } else {
      const kanji = tile.dragon === 'red' ? '中' : '發';
      const color = tile.dragon === 'red' ? COLOR.red : COLOR.green;
      ctx.fillStyle = color;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.font = `bold ${Math.floor(h * 0.62)}px "PingFang SC", "Microsoft YaHei", "SimSun", serif`;
      ctx.fillText(kanji, x + w / 2, y + h * 0.55);
    }
    ctx.restore();
  }

  function drawFlowerOrSeason(ctx, tile, x, y, w, h) {
    const isSeason = tile.category === 'season';
    const KANJI = isSeason
      ? { 1: '春', 2: '夏', 3: '秋', 4: '冬' }
      : { 1: '梅', 2: '蘭', 3: '菊', 4: '竹' };
    const NAME_MAP = {
      Plum: '梅', Orchid: '蘭', Chrysanthemum: '菊', Bamboo: '竹',
      Spring: '春', Summer: '夏', Autumn: '秋', Winter: '冬',
    };
    const rank = flowerSeasonRank(tile);
    const label = KANJI[rank] || NAME_MAP[tile.name] || '?';
    const color = isSeason ? COLOR.blue : COLOR.green;
    ctx.save();
    ctx.fillStyle = color;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.font = `bold ${Math.floor(h * 0.5)}px "PingFang SC", "Microsoft YaHei", "SimSun", serif`;
    ctx.fillText(label, x + w / 2, y + h * 0.55);
    ctx.restore();
  }

  function draw(ctx, tile, x, y, w, h, opts = {}) {
    const highlight = opts.highlight || 0;
    drawFrame(ctx, x, y, w, h, highlight);
    if (!tile) return;
    // pad before drawing content
    const px = x + 2;
    const py = y + 4;
    const pw = w - 4;
    const ph = h - 8;
    if (tile.category === 'suit' && tile.suit === 'dots') drawDots(ctx, tile, px, py, pw, ph);
    else if (tile.category === 'suit' && tile.suit === 'bamboo') drawBamboo(ctx, tile, px, py, pw, ph);
    else if (tile.category === 'suit' && tile.suit === 'characters') drawCharacters(ctx, tile, px, py, pw, ph);
    else if (tile.category === 'wind') drawWind(ctx, tile, px, py, pw, ph);
    else if (tile.category === 'dragon') drawDragon(ctx, tile, px, py, pw, ph);
    else if (tile.category === 'flower' || tile.category === 'season') drawFlowerOrSeason(ctx, tile, px, py, pw, ph);
    drawSmallIndex(ctx, tile, x, y, w);
  }

  function drawBack(ctx, x, y, w, h) {
    const r = Math.max(4, Math.min(8, Math.floor(w * 0.12)));
    ctx.fillStyle = 'rgba(0,0,0,0.35)';
    roundedRect(ctx, x + 1, y + 3, w, h, r);
    ctx.fill();
    const grad = ctx.createLinearGradient(x, y, x, y + h);
    grad.addColorStop(0, '#2b7a4d');
    grad.addColorStop(1, '#155832');
    ctx.fillStyle = grad;
    roundedRect(ctx, x, y, w, h, r);
    ctx.fill();
    ctx.strokeStyle = '#062c19';
    ctx.lineWidth = 1.5;
    roundedRect(ctx, x + 0.5, y + 0.5, w - 1, h - 1, r);
    ctx.stroke();
    // diamond ornament
    ctx.save();
    ctx.translate(x + w / 2, y + h / 2);
    ctx.rotate(Math.PI / 4);
    const s = Math.min(w, h) * 0.32;
    ctx.fillStyle = 'rgba(212, 166, 87, 0.6)';
    ctx.fillRect(-s / 2, -s / 2, s, s);
    ctx.restore();
  }

  function tileFace(tile) {
    if (!tile) return '?';
    if (tile.category === 'suit') {
      const suit = tile.suit === 'dots' ? '筒' : tile.suit === 'bamboo' ? '索' : '萬';
      return `${tile.rank}${suit}`;
    }
    if (tile.category === 'wind') return { east: '東', south: '南', west: '西', north: '北' }[tile.wind] || '?';
    if (tile.category === 'dragon') return { red: '中', green: '發', white: '白' }[tile.dragon] || '?';
    return tile.name || '?';
  }

  return { draw, drawBack, tileFace };
})();
