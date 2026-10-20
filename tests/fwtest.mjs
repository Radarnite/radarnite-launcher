// Run with the firewalled node.exe: internet must fail, loopback must work.
import net from 'node:net';
const tryConnect = (host, port) => new Promise(res => { const s = net.connect({ host, port, timeout: 5000 }); s.on('connect', () => { s.destroy(); res('CONNECTED'); }); s.on('timeout', () => { s.destroy(); res('timeout'); }); s.on('error', e => res('error ' + e.code)); });
const srv = net.createServer(c => c.end()).listen(0, '127.0.0.1', async () => {
  console.log('loopback 127.0.0.1:', await tryConnect('127.0.0.1', srv.address().port));
  console.log('1.1.1.1:443 (internet):', await tryConnect('1.1.1.1', 443));
  console.log('account-public-service-prod.ol.epicgames.com:443:', await tryConnect('account-public-service-prod.ol.epicgames.com', 443));
  srv.close();
});
