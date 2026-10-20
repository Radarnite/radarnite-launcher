import fs from 'node:fs';
const f = process.argv[2]; const b = fs.readFileSync(f);
const SP = Buffer.from([0x89,0x54,0x24,0x10,0x4C,0x89,0x44,0x24,0x18,0x4C,0x89,0x4C,0x24,0x20]);
let i = 0, hits = [];
while ((i = b.indexOf(SP, i)) >= 0) {
  const p = i; i++;
  if (!(b[p+14]===0x48&&b[p+15]===0x83&&b[p+16]===0xEC)) continue;
  let nc=false, r43=false;
  for (let k=18;k<30;k++) if (b[p+k]===0x48&&b[p+k+1]===0x85&&b[p+k+2]===0xC9){nc=true;break;}
  if(!nc) continue;
  for (let k=18;k<42;k++){ if(b[p+k]===0x8D&&b[p+k+1]===0x41&&b[p+k+2]===0x2B){r43=true;break;} if(b[p+k]===0xB8&&b[p+k+1]===0x2B&&b[p+k+2]===0&&b[p+k+3]===0&&b[p+k+4]===0){r43=true;break;} }
  if (r43) hits.push(p.toString(16)+' '+b.subarray(p,p+40).toString('hex'));
}
console.log('setopt matches', hits.length); hits.forEach(h=>console.log(' ',h));
const s16 = b.toString('utf16le'); const s8 = b.toString('latin1');
for (const re of [/\+\+Fortnite\+Release-[\d.]+-CL-\d+/g, /\d\.\d+\.\d+-\d+\+\+\+Fortnite\+Release-[\d.]+/g]) {
  console.log(re, [...new Set([...(s16.match(re)||[]), ...(s8.match(re)||[])])].slice(0,5));
}
