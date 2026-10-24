// Fake GitHub releases API for update-check.ps1 (the launcher reaches it through RADARNITE_UPDATE_API):
//   node fake-github.mjs <requests.log>
// Listens on 127.0.0.1 on a free port, printed as the first stdout line, and answers per case
//   /<case>[~label]/repos/Radarnite/radarnite-launcher/releases/latest      -> the case's latest release, or its status
//   /<case>[~label]/repos/Radarnite/radarnite-launcher/releases?per_page=N  -> the case's release list, or its status
// Every request path is appended to <requests.log>, so a test can tell which endpoints a check asked (~label keeps
// two runs of one case apart). Exits by itself after 10 minutes.
import fs from 'node:fs';
import http from 'node:http';

const repo = 'Radarnite/radarnite-launcher';
// one release as the API returns it: the installer and its .sha256, a prerelease when the version has one
const rel = (version, { prerelease = version.includes('-'), draft = false, installer = true } = {}) => {
  const tag = `v${version}`, name = `RadarniteLauncherSetup-${version}.exe`;
  const url = `https://github.com/${repo}/releases/download/${tag}/${name}`;
  const assets = [{ name: `${name}.sha256`, browser_download_url: `${url}.sha256` }];
  if (installer) assets.unshift({ name, browser_download_url: url, digest: `sha256:${'0'.repeat(64)}` });
  return { tag_name: tag, name: tag, draft, prerelease, body: `Notes of ${tag}`, assets };
};
const r221 = rel('2.2.1'), rc1 = rel('3.0.0-rc.1'), rc2 = rel('3.0.0-rc.2'), final = rel('3.0.0');

// latest: releases/latest (never a prerelease on GitHub); list: /releases, newest first. A number = that status.
const cases = {
  'stable-vs-221': { latest: r221, list: [rc2, rc1, r221] },
  'stable-vs-301': { latest: rel('3.0.1'), list: [rel('3.0.2-rc.1'), rel('3.0.1')] },
  'stable-vs-mislabelled': { latest: rel('3.0.1-hotfix.1', { prerelease: false }), list: [rel('3.0.1-hotfix.1', { prerelease: false })] },
  'rc1-vs-rc2': { latest: r221, list: [rc2, rc1, r221] },
  'rc2-vs-final': { latest: final, list: [final, rc2, rc1, r221] },
  'rc2-draft-rc3': { latest: r221, list: [rel('3.0.0-rc.3', { draft: true }), rc2, rc1, r221] },
  'rc2-vs-older-rc1': { latest: r221, list: [rc1, r221] },
  'rc2-rc3-no-installer': { latest: r221, list: [rel('3.0.0-rc.3', { installer: false }), rc2, rc1] },
  'rc1-list-500': { latest: r221, list: 500 },
  'rc1-no-latest': { latest: 404, list: [rc2, rc1] },
  'rc1-newer-rc-than-final': { latest: final, list: [rel('3.0.1-rc.1'), final, rc2, rc1] },
};

const log = process.argv[2];
const route = new RegExp(`^/([^/~]+)(?:~[^/]*)?/repos/${repo}/releases(/latest|\\?per_page=\\d+)$`);
const server = http.createServer((req, res) => {
  if (log) fs.appendFileSync(log, `${req.url}\n`);
  const m = req.url.match(route);
  const c = m && cases[m[1]];
  const body = c ? (m[2] === '/latest' ? c.latest : c.list) : 404;
  const status = typeof body === 'number' ? body : 200;
  res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
  res.end(status === 200 ? JSON.stringify(body) : JSON.stringify({ message: http.STATUS_CODES[status] }));
});
server.listen(0, '127.0.0.1', () => console.log(server.address().port));
setTimeout(() => process.exit(0), 10 * 60 * 1000);
