// Compares the native selftest PNGs with the page rendered by Chromium for the same scenarios, inside Chromium (canvas).
// usage: node compare.mjs <native dir> <out dir> <page dir> [page dir...]   (PW_DIR = folder with node_modules/playwright)
// For each <id>.png present in the native dir and in a page dir it writes to <out dir>:
//   <id>-pair.png    page | native, scaled down to fit 1280x720
//   <id>-diff.png    native dimmed, differing pixels in red, scaled down to fit 1280x720
//   <id>-rN.png      the N-th largest differing region, page | native side by side at 1:1 (2x when small), <= 1280x720
//   <id>-cN.png      CMP_CROPS="id:x,y,w,h,zoom;..." (image pixels): page | native of that box, zoomed, <= 1280x720
//   compare.json     per scenario: size, differing pixels (% of the image), region boxes in image pixels and the
//                    shift of the native image that best matches the page in each region; colorPct/colorRegions:
//                    flat areas (in both images) whose colour differs by more than 6 on a channel, with the mean
//                    difference native - page (catches wrong backgrounds and tints the main mask is blind to)
// Both images are blurred (3x3 box) first, then a pixel differs when no pixel within 1 px of it in the other image is
// within 40 of it on every channel: ignores antialiasing, glyph gamma and sub-pixel placement (Chromium and DirectWrite
// rasterize text differently); finds anything that moved by 2 px or more or changed colour.
import { createRequire } from 'node:module';
import path from 'node:path';
import fs from 'node:fs';
const require = createRequire(process.env.PW_DIR || import.meta.url);
const { chromium } = require('playwright');
const [nativeDir, out, ...pageDirs] = process.argv.slice(2);
fs.mkdirSync(out, { recursive: true });

const pairs = [];
for (const f of fs.readdirSync(nativeDir).filter((f) => f.endsWith('.png') && !f.startsWith('00-')).sort()) {
  const p = pageDirs.map((d) => path.join(d, f)).find((x) => fs.existsSync(x));
  if (p) pairs.push({ id: f.slice(0, -4), page: p, native: path.join(nativeDir, f) });
}
const dataUrl = (f) => 'data:image/png;base64,' + fs.readFileSync(f).toString('base64');

function inPage({ a, b, maxRegions, extra }) {
  const load = (src) => new Promise((ok, bad) => { const i = new Image(); i.onload = () => ok(i); i.onerror = bad; i.src = src; });
  const canvas = (w, h) => { const c = document.createElement('canvas'); c.width = w; c.height = h; return c; };
  const pixels = (img) => { const c = canvas(img.width, img.height), x = c.getContext('2d'); x.drawImage(img, 0, 0); return x.getImageData(0, 0, img.width, img.height); };
  const fit = (w, h, mw, mh) => Math.min(1, mw / w, mh / h);
  return Promise.all([load(a), load(b)]).then(([ia, ib]) => {
    const blur = (P) => {   // 3x3 box, RGB
      const o = new ImageData(P.width, P.height), W = P.width, H = P.height, d = P.data;
      for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        let r = 0, g = 0, b = 0, n = 0;
        for (let dy = -1; dy <= 1; dy++) { const yy = y + dy; if (yy < 0 || yy >= H) continue;
          for (let dx = -1; dx <= 1; dx++) { const xx = x + dx; if (xx < 0 || xx >= W) continue; const j = (yy * W + xx) * 4; r += d[j]; g += d[j + 1]; b += d[j + 2]; n++; } }
        const i = (y * W + x) * 4; o.data[i] = r / n; o.data[i + 1] = g / n; o.data[i + 2] = b / n; o.data[i + 3] = 255;
      }
      return o;
    };
    const B = pixels(ib), A0 = pixels(ia), Ab = blur(A0), Bb = blur(B);
    const w = Math.min(A0.width, B.width), h = Math.min(A0.height, B.height);
    const T = 40;
    const near = (P, Q, x, y) => {   // is P(x,y) matched by a pixel of Q within 1 px?
      const i = (y * P.width + x) * 4;
      for (let dy = -1; dy <= 1; dy++) {
        const yy = y + dy; if (yy < 0 || yy >= h) continue;
        for (let dx = -1; dx <= 1; dx++) {
          const xx = x + dx; if (xx < 0 || xx >= w) continue;
          const j = (yy * Q.width + xx) * 4;
          if (Math.abs(P.data[i] - Q.data[j]) <= T && Math.abs(P.data[i + 1] - Q.data[j + 1]) <= T && Math.abs(P.data[i + 2] - Q.data[j + 2]) <= T) return true;
        }
      }
      return false;
    };
    const mask = new Uint8Array(w * h);
    let n = 0;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) if (!near(Ab, Bb, x, y) || !near(Bb, Ab, x, y)) { mask[y * w + x] = 1; n++; }
    // regions: 8 px cells with at least 3 differing pixels, joined when they touch (one empty cell of tolerance)
    const C = 8, cw = Math.ceil(w / C), ch = Math.ceil(h / C), cells = new Uint32Array(cw * ch);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) if (mask[y * w + x]) cells[((y / C) | 0) * cw + ((x / C) | 0)]++;
    const seen = new Uint8Array(cw * ch), regions = [];
    for (let s = 0; s < cw * ch; s++) {
      if (seen[s] || cells[s] < 3) continue;
      let x0 = 1e9, y0 = 1e9, x1 = -1, y1 = -1, cnt = 0;
      const st = [s]; seen[s] = 1;
      while (st.length) {
        const c = st.pop(), cx = c % cw, cy = (c / cw) | 0;
        x0 = Math.min(x0, cx); y0 = Math.min(y0, cy); x1 = Math.max(x1, cx); y1 = Math.max(y1, cy); cnt += cells[c];
        for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) {
          const nx = cx + dx, ny = cy + dy; if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
          const k = ny * cw + nx; if (!seen[k] && cells[k] >= 3) { seen[k] = 1; st.push(k); }
        }
      }
      regions.push({ x: x0 * C, y: y0 * C, w: Math.min(w, (x1 + 1) * C) - x0 * C, h: Math.min(h, (y1 + 1) * C) - y0 * C, n: cnt });
    }
    regions.sort((p, q) => q.n - p.n);
    // colour-only differences on flat areas
    const flat = (P, x, y) => {
      const i = (y * P.width + x) * 4;
      for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
        const xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
        const j = (yy * P.width + xx) * 4;
        if (Math.abs(P.data[i] - P.data[j]) > 2 || Math.abs(P.data[i + 1] - P.data[j + 1]) > 2 || Math.abs(P.data[i + 2] - P.data[j + 2]) > 2) return false;
      }
      return true;
    };
    const cmask = new Uint8Array(w * h);
    let cn = 0;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const i = (y * Ab.width + x) * 4, j = (y * Bb.width + x) * 4;
      const d0 = Bb.data[j] - Ab.data[i], d1 = Bb.data[j + 1] - Ab.data[i + 1], d2 = Bb.data[j + 2] - Ab.data[i + 2];
      if ((Math.abs(d0) > 6 || Math.abs(d1) > 6 || Math.abs(d2) > 6) && flat(Ab, x, y) && flat(Bb, x, y)) { cmask[y * w + x] = 1; cn++; }
    }
    const ccells = new Uint32Array(cw * ch);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) if (cmask[y * w + x]) ccells[((y / C) | 0) * cw + ((x / C) | 0)]++;
    const cseen = new Uint8Array(cw * ch), colorRegions = [];
    for (let s0 = 0; s0 < cw * ch; s0++) {
      if (cseen[s0] || ccells[s0] < 16) continue;
      let x0 = 1e9, y0 = 1e9, x1 = -1, y1 = -1, cnt = 0;
      const st = [s0]; cseen[s0] = 1;
      while (st.length) {
        const c = st.pop(), cx = c % cw, cy = (c / cw) | 0;
        x0 = Math.min(x0, cx); y0 = Math.min(y0, cy); x1 = Math.max(x1, cx); y1 = Math.max(y1, cy); cnt += ccells[c];
        for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
          const nx = cx + dx, ny = cy + dy; if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
          const k = ny * cw + nx; if (!cseen[k] && ccells[k] >= 16) { cseen[k] = 1; st.push(k); }
        }
      }
      const r = { x: x0 * C, y: y0 * C, w: Math.min(w, (x1 + 1) * C) - x0 * C, h: Math.min(h, (y1 + 1) * C) - y0 * C, n: cnt };
      let s3 = [0, 0, 0], m = 0, pc = [0, 0, 0];
      for (let y = r.y; y < r.y + r.h; y++) for (let x = r.x; x < r.x + r.w; x++) if (cmask[y * w + x]) {
        const i = (y * Ab.width + x) * 4; m++;
        for (let k = 0; k < 3; k++) { s3[k] += Bb.data[i + k] - Ab.data[i + k]; pc[k] += Ab.data[i + k]; }
      }
      r.delta = s3.map((v) => Math.round(v / m));
      r.page = '#' + pc.map((v) => Math.round(v / m).toString(16).padStart(2, '0')).join('');
      colorRegions.push(r);
    }
    colorRegions.sort((p, q) => q.n - p.n);
    // per region: the shift of the native image that matches the page best (sum of abs differences, +-12 px)
    const sad = (r, ox, oy) => {
      let t = 0, n = 0;
      for (let y = r.y; y < r.y + r.h; y += 2) for (let x = r.x; x < r.x + r.w; x += 2, n++) {
        const xx = x + ox, yy = y + oy;
        if (xx < 0 || yy < 0 || xx >= w || yy >= h) { t += 255; continue; }
        const i = (y * Ab.width + x) * 4, j = (yy * Bb.width + xx) * 4;
        t += Math.abs(Ab.data[i] - Bb.data[j]) + Math.abs(Ab.data[i + 1] - Bb.data[j + 1]) + Math.abs(Ab.data[i + 2] - Bb.data[j + 2]);
      }
      return t / n;
    };
    for (const r of regions.slice(0, 10)) {
      let best = [0, 0], bv = sad(r, 0, 0);
      const v0 = bv;
      for (let oy = -12; oy <= 12; oy++) for (let ox = -12; ox <= 12; ox++) { const v = sad(r, ox, oy); if (v < bv - 0.01) { bv = v; best = [ox, oy]; } }
      r.shift = best; r.err = [+v0.toFixed(2), +bv.toFixed(2)];   // native content at page + shift; error before/after
    }
    const png = (c) => c.toDataURL('image/png');
    // page | native overview
    const s1 = fit(ia.width + ib.width + 8, Math.max(ia.height, ib.height), 1280, 720);
    const pc = canvas(Math.round((ia.width + ib.width + 8) * s1), Math.round(Math.max(ia.height, ib.height) * s1)), px = pc.getContext('2d');
    px.fillStyle = '#f0f'; px.fillRect(0, 0, pc.width, pc.height);
    px.drawImage(ia, 0, 0, ia.width * s1, ia.height * s1); px.drawImage(ib, (ia.width + 8) * s1, 0, ib.width * s1, ib.height * s1);
    // diff map on the native image
    const dm = new ImageData(w, h);
    for (let i = 0; i < w * h; i++) {
      const j = (Math.floor(i / w) * B.width + (i % w)) * 4;
      if (mask[i]) { dm.data[i * 4] = 255; dm.data[i * 4 + 1] = 0; dm.data[i * 4 + 2] = 0; }
      else { dm.data[i * 4] = B.data[j] * 0.35; dm.data[i * 4 + 1] = B.data[j + 1] * 0.35; dm.data[i * 4 + 2] = B.data[j + 2] * 0.35; }
      dm.data[i * 4 + 3] = 255;
    }
    const dfull = canvas(w, h); dfull.getContext('2d').putImageData(dm, 0, 0);
    const s2 = fit(w, h, 1280, 720), dc = canvas(Math.round(w * s2), Math.round(h * s2));
    dc.getContext('2d').drawImage(dfull, 0, 0, dc.width, dc.height);
    // largest regions side by side at 1:1 (2x when small), each half at most 636x720
    const crops = regions.filter((r) => r.n >= 20).slice(0, maxRegions).map((r) => {
      const pad = 16;
      let x = Math.max(0, r.x - pad), y = Math.max(0, r.y - pad);
      let rw = Math.min(w - x, r.w + 2 * pad), rh = Math.min(h - y, r.h + 2 * pad);
      const z = rw <= 300 && rh <= 340 ? 2 : 1;
      rw = Math.min(rw, Math.floor(636 / z)); rh = Math.min(rh, Math.floor(720 / z));
      const c = canvas(rw * z * 2 + 8, rh * z), x2 = c.getContext('2d');
      x2.imageSmoothingEnabled = false;
      x2.fillStyle = '#f0f'; x2.fillRect(0, 0, c.width, c.height);
      x2.drawImage(ia, x, y, rw, rh, 0, 0, rw * z, rh * z);
      x2.drawImage(ib, x, y, rw, rh, rw * z + 8, 0, rw * z, rh * z);
      return { box: [x, y, rw, rh], zoom: z, png: png(c) };
    });
    const zoomed = extra.map(([x, y, rw, rh, z]) => {
      z = Math.max(1, Math.min(z || 1, Math.floor(636 / rw), Math.floor(720 / rh)));
      const c = canvas(rw * z * 2 + 8, rh * z), x2 = c.getContext('2d');
      x2.imageSmoothingEnabled = false;
      x2.fillStyle = '#f0f'; x2.fillRect(0, 0, c.width, c.height);
      x2.drawImage(ia, x, y, rw, rh, 0, 0, rw * z, rh * z);
      x2.drawImage(ib, x, y, rw, rh, rw * z + 8, 0, rw * z, rh * z);
      return png(c);
    });
    return { zoomed, colorPct: +(cn * 100 / (w * h)).toFixed(3), colorRegions: colorRegions.slice(0, 8), size: [A0.width, A0.height, B.width, B.height], diffPct: +(n * 100 / (w * h)).toFixed(3), diffPx: n,
             regions: regions.slice(0, 16), pair: png(pc), diff: png(dc), crops };
  });
}

const extraCrops = {};
for (const c of (process.env.CMP_CROPS || '').split(';').filter(Boolean)) {
  const [id, box] = c.split(':'); (extraCrops[id] ||= []).push(box.split(',').map(Number));
}
const browser = await chromium.launch({ headless: true });
const report = [];
try {
  const page = await (await browser.newContext()).newPage();
  await page.setContent('<!doctype html><title>compare</title>');
  for (const p of pairs) {
    const r = await page.evaluate(inPage, { a: dataUrl(p.page), b: dataUrl(p.native), maxRegions: 4, extra: extraCrops[p.id] || [] });
    const save = (name, url) => { fs.writeFileSync(path.join(out, name), Buffer.from(url.split(',')[1], 'base64')); return name; };
    const files = [save(`${p.id}-pair.png`, r.pair), save(`${p.id}-diff.png`, r.diff)];
    r.crops.forEach((c, i) => { files.push(save(`${p.id}-r${i + 1}.png`, c.png)); delete c.png; });
    r.zoomed.forEach((u, i) => files.push(save(`${p.id}-c${i + 1}.png`, u)));
    const sizeMatch = r.size[0] === r.size[2] && r.size[1] === r.size[3];
    report.push({ id: p.id, size: `${r.size[0]}x${r.size[1]}`, sizeMatch, diffPct: r.diffPct, diffPx: r.diffPx, colorPct: r.colorPct,
                  regions: r.regions, colorRegions: r.colorRegions, crops: r.crops, files });
    console.log(`${p.id.padEnd(36)} ${String(r.diffPct).padStart(7)} %  colour ${String(r.colorPct).padStart(6)} %  ${sizeMatch ? '' : 'SIZE ' + r.size.join('x') + '  '}regions ${r.regions.length}`);
  }
} finally { await browser.close(); }
fs.writeFileSync(path.join(out, 'compare.json'), JSON.stringify(report, null, 1));
