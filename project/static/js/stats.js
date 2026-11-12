import { el } from './api.js?v=dash-a-6';

// Trang Biểu đồ: 3 biểu đồ SVG vẽ tay (không cần thư viện, chạy được khi mất Internet).
const SVG = 'http://www.w3.org/2000/svg';
const OUTCOMES = [
  { key: 'ok', label: 'Nhận diện được', cls: 'ok' },
  { key: 'unknown', label: 'Chưa rõ', cls: 'warn' },
  { key: 'no_hand', label: 'Không thấy tay', cls: 'bad' },
];
const SOURCES = [
  { key: 'esp32', label: 'ESP32' },
  { key: 'browser', label: 'Camera máy này' },
  { key: 'upload', label: 'Tải ảnh' },
];
const sourceLabel = (key) => SOURCES.find((s) => s.key === key)?.label || key;
const outcomeLabel = (key) => OUTCOMES.find((o) => o.key === key)?.label || key;
const pct = (value) =>
  (value * 100).toLocaleString('vi-VN', { maximumFractionDigits: value < 0.1 ? 1 : 0 }) + '%';
const two = (n) => String(n).padStart(2, '0');
const clock = (iso) => {
  const d = new Date(iso);
  return `${two(d.getHours())}:${two(d.getMinutes())} · ${two(d.getDate())}/${two(d.getMonth() + 1)}`;
};
const seconds = (ms) =>
  (ms / 1000).toLocaleString('vi-VN', { maximumFractionDigits: ms < 1000 ? 2 : 1 }) + ' s';

function svg(tag, attrs = {}, parent) {
  const node = document.createElementNS(SVG, tag);
  for (const [k, v] of Object.entries(attrs)) node.setAttribute(k, v);
  parent?.append(node);
  return node;
}
function empty(box, text = 'Chưa có dữ liệu trong khoảng thời gian này.') {
  const p = document.createElement('p');
  p.className = 'chart-empty';
  p.textContent = text;
  box.replaceChildren(p);
}
function table(id, head, rows) {
  const t = el(id);
  t.replaceChildren();
  const tr = t.createTHead().insertRow();
  for (const h of head) {
    const th = document.createElement('th');
    th.textContent = h;
    tr.append(th);
  }
  const body = t.createTBody();
  for (const r of rows) {
    const row = body.insertRow();
    for (const c of r) row.insertCell().textContent = c;
  }
}

export class StatsView {
  constructor(api) {
    this.api = api;
    this.range = 'today';
    this.open = false;
    this.data = null;
    this.timer = null;
    this.debounce = null;
    this.loading = false;
    this.tip = el('chart-tooltip');
    for (const b of document.querySelectorAll('#stats-view [data-range]'))
      b.onclick = () => this.setRange(b.dataset.range);
    window.addEventListener('resize', () => this.open && this.data && this.render());
  }
  show() {
    this.open = true;
    el('stats-view').hidden = false;
    this.refresh();
    clearInterval(this.timer);
    this.timer = setInterval(() => this.refresh(), 15000); // du phong neu mat luong su kien
  }
  hide() {
    this.open = false;
    el('stats-view').hidden = true;
    clearInterval(this.timer);
    this.hideTip();
  }
  setRange(range) {
    this.range = range;
    for (const b of document.querySelectorAll('#stats-view [data-range]'))
      b.setAttribute('aria-pressed', String(b.dataset.range === range));
    this.refresh();
  }
  // Goi khi co ket qua moi (luong su kien realtime).
  onEvent() {
    if (!this.open) return;
    clearTimeout(this.debounce);
    this.debounce = setTimeout(() => this.refresh(), 400);
  }
  async refresh() {
    if (!this.open || this.loading) return;
    this.loading = true;
    try {
      this.data = await this.api.json('/api/stats?range=' + this.range);
      this.render();
      el('stats-updated').textContent =
        'Trực tiếp · cập nhật ' +
        new Date().toLocaleTimeString('vi-VN', {
          hour: '2-digit',
          minute: '2-digit',
          second: '2-digit',
        });
      el('stats-updated').dataset.state = 'ok';
    } catch (error) {
      el('stats-updated').textContent = 'Không tải được thống kê: ' + error.message;
      el('stats-updated').dataset.state = 'off';
    } finally {
      this.loading = false;
    }
  }
  render() {
    this.hideTip();
    this.donut();
    this.sources();
    this.latency();
  }

  // ---------- Tooltip ----------
  showTip(event, lines) {
    const tip = this.tip;
    tip.replaceChildren(
      ...lines.map((line, i) => {
        const div = document.createElement('div');
        div.textContent = line;
        if (i === 0) div.className = 'tip-title';
        return div;
      }),
    );
    tip.hidden = false;
    const pad = 14,
      w = tip.offsetWidth,
      h = tip.offsetHeight;
    let x = event.clientX + pad,
      y = event.clientY + pad;
    if (x + w > window.innerWidth - 8) x = event.clientX - w - pad;
    if (y + h > window.innerHeight - 8) y = event.clientY - h - pad;
    tip.style.left = Math.max(8, x) + 'px';
    tip.style.top = Math.max(8, y) + 'px';
  }
  hideTip() {
    if (this.tip) this.tip.hidden = true;
  }
  hover(node, lines) {
    node.addEventListener('pointermove', (e) => this.showTip(e, lines));
    node.addEventListener('pointerleave', () => this.hideTip());
  }

  // ---------- 1. Donut: ti le nhan dien ----------
  donut() {
    const box = el('chart-donut'),
      legend = el('donut-legend'),
      { outcomes, total, success_rate: rate } = this.data;
    legend.replaceChildren();
    table(
      'table-donut',
      ['Kết quả', 'Số lần', 'Tỉ lệ'],
      OUTCOMES.map((o) => [o.label, outcomes[o.key], total ? pct(outcomes[o.key] / total) : '—']),
    );
    if (!total) return empty(box);
    const size = 200,
      r = 80,
      stroke = 30,
      c = size / 2,
      circ = 2 * Math.PI * r;
    const root = svg('svg', {
      viewBox: `0 0 ${size} ${size}`,
      role: 'img',
      'aria-label': `Tỉ lệ nhận diện được ${pct(rate)} trên ${total} lần`,
    });
    svg('circle', { cx: c, cy: c, r, class: 'donut-track', 'stroke-width': stroke }, root);
    let offset = 0;
    const gap = OUTCOMES.filter((o) => outcomes[o.key]).length > 1 ? 2 : 0; // khe 2px giua cac phan
    for (const o of OUTCOMES) {
      const value = outcomes[o.key];
      if (!value) continue;
      const len = (value / total) * circ;
      const arc = svg(
        'circle',
        {
          cx: c,
          cy: c,
          r,
          class: 'seg ' + o.cls,
          'stroke-width': stroke,
          'stroke-dasharray': `${Math.max(0, len - gap)} ${circ}`,
          'stroke-dashoffset': -offset,
          transform: `rotate(-90 ${c} ${c})`,
        },
        root,
      );
      this.hover(arc, [o.label, `${value} lần · ${pct(value / total)}`]);
      offset += len;
    }
    svg('text', { x: c, y: c + 4, class: 'donut-big', 'text-anchor': 'middle' }, root).textContent =
      pct(rate);
    svg(
      'text',
      { x: c, y: c + 26, class: 'donut-small', 'text-anchor': 'middle' },
      root,
    ).textContent = `trên ${total} lần`;
    box.replaceChildren(root);
    for (const o of OUTCOMES) {
      const li = document.createElement('li'),
        sw = document.createElement('span'),
        name = document.createElement('span'),
        val = document.createElement('strong');
      sw.className = 'swatch ' + o.cls;
      name.textContent = o.label;
      val.textContent = `${outcomes[o.key]} · ${pct(outcomes[o.key] / total)}`;
      li.append(sw, name, val);
      legend.append(li);
    }
  }

  // ---------- 2. Cot chong 100%: ket qua theo nguon ----------
  sources() {
    const box = el('chart-sources'),
      by = this.data.by_source;
    table(
      'table-sources',
      ['Nguồn', 'Tổng', ...OUTCOMES.map((o) => o.label)],
      SOURCES.map((s) => {
        const row = by[s.key],
          total = OUTCOMES.reduce((n, o) => n + row[o.key], 0);
        return [
          s.label,
          total,
          ...OUTCOMES.map((o) => (total ? `${row[o.key]} (${pct(row[o.key] / total)})` : '—')),
        ];
      }),
    );
    if (!this.data.total) return empty(box);
    const width = Math.max(280, box.clientWidth || 480),
      labelW = 128,
      barH = 28,
      rowH = 56,
      plotW = width - labelW - 8,
      height = SOURCES.length * rowH;
    const root = svg('svg', {
      viewBox: `0 0 ${width} ${height}`,
      width,
      height,
      role: 'img',
      'aria-label': 'Tỉ lệ kết quả theo từng nguồn ảnh',
    });
    SOURCES.forEach((s, i) => {
      const row = by[s.key],
        total = OUTCOMES.reduce((n, o) => n + row[o.key], 0),
        y = i * rowH + 8;
      svg('text', { x: 0, y: y + 12, class: 'axis-label strong' }, root).textContent = s.label;
      svg('text', { x: 0, y: y + 30, class: 'axis-label' }, root).textContent = `${total} lần`;
      if (!total) {
        svg('rect', { x: labelW, y, width: plotW, height: barH, rx: 4, class: 'bar-empty' }, root);
        svg('text', { x: labelW + 10, y: y + 19, class: 'axis-label' }, root).textContent =
          'Chưa có dữ liệu';
        return;
      }
      const parts = OUTCOMES.filter((o) => row[o.key]);
      let x = labelW;
      parts.forEach((o, j) => {
        const share = row[o.key] / total,
          w = share * plotW,
          last = j === parts.length - 1,
          drawW = Math.max(1, w - (last ? 0 : 2)); // khe 2px giua cac doan
        const first = j === 0;
        const rect = svg(
          'path',
          {
            d: roundedBar(x, y, drawW, barH, first ? 4 : 0, last ? 4 : 0),
            class: 'seg-fill ' + o.cls,
          },
          root,
        );
        this.hover(rect, [
          `${s.label} · ${o.label}`,
          `${row[o.key]} / ${total} lần · ${pct(share)}`,
        ]);
        if (drawW > 44) {
          const t = svg(
            'text',
            { x: x + drawW / 2, y: y + 19, 'text-anchor': 'middle', class: 'in-bar ' + o.cls },
            root,
          );
          t.textContent = pct(share);
        }
        x += w;
      });
    });
    box.replaceChildren(root);
  }

  // ---------- 3. Duong: thoi gian xu ly ----------
  latency() {
    const box = el('chart-latency'),
      points = this.data.latency;
    table(
      'table-latency',
      ['Thời điểm', 'Nguồn', 'Kết quả', 'Thời gian xử lý'],
      [...points]
        .reverse()
        .map((p) => [clock(p.time), sourceLabel(p.source), outcomeLabel(p.outcome), seconds(p.ms)]),
    );
    const hero = el('latency-hero');
    if (!points.length) {
      hero.textContent = '';
      return empty(box);
    }
    const sorted = points.map((p) => p.ms).sort((a, b) => a - b),
      median = sorted[Math.floor(sorted.length / 2)];
    hero.replaceChildren();
    const big = document.createElement('strong'),
      small = document.createElement('span');
    big.textContent = seconds(median);
    small.textContent = 'trung vị';
    hero.append(big, small);

    const width = Math.max(300, box.clientWidth || 800),
      height = window.innerHeight < 860 ? 180 : 240,
      m = { l: 52, r: 16, t: 14, b: 30 },
      pw = width - m.l - m.r,
      ph = height - m.t - m.b;
    const max = niceMax(sorted[sorted.length - 1]),
      n = points.length;
    const px = (i) => m.l + (n === 1 ? pw / 2 : (i / (n - 1)) * pw),
      py = (ms) => m.t + ph - (ms / max) * ph;
    const root = svg('svg', {
      viewBox: `0 0 ${width} ${height}`,
      width,
      height,
      role: 'img',
      'aria-label': `Thời gian xử lý ${n} lần gần nhất, trung vị ${seconds(median)}`,
    });
    for (let k = 0; k <= 4; k++) {
      const v = (max / 4) * k,
        y = py(v);
      svg('line', { x1: m.l, x2: width - m.r, y1: y, y2: y, class: k ? 'grid' : 'baseline' }, root);
      svg(
        'text',
        { x: m.l - 8, y: y + 4, 'text-anchor': 'end', class: 'axis-label' },
        root,
      ).textContent = seconds(v);
    }
    svg('text', { x: m.l, y: height - 8, class: 'axis-label' }, root).textContent = clock(
      points[0].time,
    );
    if (n > 1)
      svg(
        'text',
        { x: width - m.r, y: height - 8, 'text-anchor': 'end', class: 'axis-label' },
        root,
      ).textContent = clock(points[n - 1].time);
    const d = points
      .map((p, i) => `${i ? 'L' : 'M'}${px(i).toFixed(1)},${py(p.ms).toFixed(1)}`)
      .join('');
    svg('path', { d, class: 'line' }, root);
    if (n <= 20)
      points.forEach((p, i) =>
        svg('circle', { cx: px(i), cy: py(p.ms), r: 3, class: 'dot' }, root),
      );

    // Crosshair + tooltip theo diem gan nhat
    const cross = svg(
      'line',
      { y1: m.t, y2: m.t + ph, class: 'crosshair', visibility: 'hidden' },
      root,
    );
    const mark = svg('circle', { r: 5, class: 'dot active', visibility: 'hidden' }, root);
    const hit = svg('rect', { x: m.l, y: m.t, width: pw, height: ph, class: 'hit' }, root);
    hit.addEventListener('pointermove', (e) => {
      const rect = root.getBoundingClientRect(),
        x = ((e.clientX - rect.left) / rect.width) * width;
      const i = n === 1 ? 0 : Math.max(0, Math.min(n - 1, Math.round(((x - m.l) / pw) * (n - 1))));
      const p = points[i];
      cross.setAttribute('x1', px(i));
      cross.setAttribute('x2', px(i));
      mark.setAttribute('cx', px(i));
      mark.setAttribute('cy', py(p.ms));
      cross.setAttribute('visibility', 'visible');
      mark.setAttribute('visibility', 'visible');
      this.showTip(e, [
        clock(p.time),
        `${seconds(p.ms)} · ${sourceLabel(p.source)}`,
        outcomeLabel(p.outcome),
      ]);
    });
    hit.addEventListener('pointerleave', () => {
      cross.setAttribute('visibility', 'hidden');
      mark.setAttribute('visibility', 'hidden');
      this.hideTip();
    });
    box.replaceChildren(root);
  }
}

function niceMax(value) {
  if (value <= 0) return 100;
  const step = 10 ** Math.floor(Math.log10(value));
  for (const k of [1, 2, 2.5, 5, 10]) if (k * step >= value) return k * step;
  return 10 * step;
}

// Thanh ngang bo goc 4px chi o dau/cuoi (left/right radius).
function roundedBar(x, y, w, h, rl, rr) {
  rl = Math.min(rl, w / 2, h / 2);
  rr = Math.min(rr, w / 2, h / 2);
  return [
    `M${x + rl},${y}`,
    `H${x + w - rr}`,
    rr ? `A${rr},${rr} 0 0 1 ${x + w},${y + rr}` : '',
    `V${y + h - rr}`,
    rr ? `A${rr},${rr} 0 0 1 ${x + w - rr},${y + h}` : '',
    `H${x + rl}`,
    rl ? `A${rl},${rl} 0 0 1 ${x},${y + h - rl}` : '',
    `V${y + rl}`,
    rl ? `A${rl},${rl} 0 0 1 ${x + rl},${y}` : '',
    'Z',
  ].join('');
}
