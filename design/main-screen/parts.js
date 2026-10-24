// Shared markup for the mockups: title bar and icons. Plain DOM, no libraries.
const ICONS = {
  cos: '<path d="M5.5 2.5 3 4v3l2-.5V13.5h6V6.5l2 .5V4l-2.5-1.5C10 3.7 9.1 4.3 8 4.3S6 3.7 5.5 2.5z"/>',
  more: '<circle cx="3.5" cy="8" r=".9" fill="currentColor"/><circle cx="8" cy="8" r=".9" fill="currentColor"/><circle cx="12.5" cy="8" r=".9" fill="currentColor"/>',
  plus: '<path d="M8 3v10M3 8h10"/>',
  log: '<path d="M3 4.5h10M3 8h10M3 11.5h6"/>',
  chev: '<path d="M6 3.5 10.5 8 6 12.5"/>',
  chevl: '<path d="M10 3.5 5.5 8 10 12.5"/>',
  down: '<path d="M3.5 6 8 10.5 12.5 6"/>',
  folder: '<path d="M2 4.5h4l1.5 1.5H14v6.5H2z"/>',
  cog: '<circle cx="8" cy="8" r="2.2"/><path d="M8 1.8v2M8 12.2v2M1.8 8h2M12.2 8h2M3.6 3.6 5 5M11 11l1.4 1.4M3.6 12.4 5 11M11 5l1.4-1.4"/>',
  user: '<circle cx="8" cy="5.5" r="2.5"/><path d="M3 13.5c.6-2.6 2.6-4 5-4s4.4 1.4 5 4"/>',
  match: '<path d="M8 1.8 13 4v4c0 3-2.2 5.2-5 6.2C5.2 13.2 3 11 3 8V4z"/>',
  lock: '<rect x="3.5" y="7" width="9" height="6.5" rx="1.2"/><path d="M5.5 7V5a2.5 2.5 0 0 1 5 0v2"/>',
  bots: '<rect x="3" y="5" width="10" height="7.5" rx="2"/><path d="M8 2.5V5M6 8.5h.01M10 8.5h.01"/>',
  play: '<path d="M4.5 2.6v10.8L13.2 8z" fill="currentColor" stroke="none"/>',
  build: '<path d="M2.5 13.5h11M4 13.5V7l4-3.5L12 7v6.5M6.5 13.5V10h3v3.5"/>',
  zip: '<path d="M4 1.8h6l2.5 2.5v9.9H4zM7 2v1.5M7 4.5V6M7 7v1.5"/>',
  import: '<path d="M8 2v8M4.8 7 8 10.2 11.2 7M3 13.5h10"/>',
  check: '<path d="m3.5 8.2 3 3 6-6.4"/>',
};
function icon(name, cls = 'ic', style = '') { return `<svg class="${cls}" style="${style}" viewBox="0 0 16 16">${ICONS[name] || ''}</svg>`; }
function titleBar() {
  return `<div class="tb"><div class="mark"><img src="../../launcher/ui/radarnite.png" alt="">Radarnite Launcher</div>
    <div class="safe"><i></i>Offline · 127.0.0.1 only</div>
    <div class="wc"><span><svg viewBox="0 0 10 10"><path d="M0 5h10"/></svg></span><span><svg viewBox="0 0 10 10"><rect x=".5" y="2.5" width="7" height="7"/><path d="M2.5 2.5V.5h7v7h-2"/></svg></span><span><svg viewBox="0 0 10 10"><path d="M0 0l10 10M10 0 0 10"/></svg></span></div></div>`;
}
// Real builds (Diego's S:\FortniteBuilds; CL/UE/cosmetics of 3.6 and 17.50 from the mock core's mockup scenario)
const BUILDS = [
  { v: '3.6', ch: 'C1 · S3', season: 'Chapter 1 · Season 3', name: 'Space Explorers', skin: 'cid_084_athena_commando_m_assassin', skinName: 'The Reaper', cl: '4019403', ue: '4.20', cos: '2,210', c: ['#A06BFF', '#5A2FB0', '#2C1866', '#0E0A24'] },
  { v: '14.30', ch: 'C2 · S4', season: 'Chapter 2 · Season 4', name: 'Nexus War', skin: 'cid_845_athena_commando_m_hightowertapas', skinName: 'Thor', cl: '14456520', ue: '4.26', cos: '4,025', c: ['#5D86FF', '#2A3F9E', '#152A6E', '#0A1028'], sel: true },
  { v: '17.50', ch: 'C2 · S7', season: 'Chapter 2 · Season 7', name: 'Invasion', skin: 'cid_a_110_athena_commando_m_alientrooper', skinName: 'Kymera', cl: '17388565', ue: '4.26', cos: '5,338', c: ['#3FE0D0', '#178A8F', '#0C4A55', '#05181F'] },
];
const art = (b) => `art/${b.skin}.png`;
document.addEventListener('DOMContentLoaded', () => {
  document.querySelectorAll('[data-tb]').forEach((e) => { e.outerHTML = titleBar(); });
  document.querySelectorAll('[data-i]').forEach((e) => { e.outerHTML = icon(e.dataset.i, e.className || 'ic', e.getAttribute('style') || ''); });
});
