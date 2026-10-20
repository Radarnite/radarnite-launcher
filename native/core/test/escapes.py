import sys
text = open(sys.argv[1], encoding='utf-8').read()
key = '"practicePlaylist": "'
i = text.index(key) + len(key)
esc = set(); raw = set()
while text[i] != '"':
    if text[i] == '\\':
        c = text[i+1]
        if c == 'u':
            cp = int(text[i+2:i+6], 16); i += 6
            if 0xD800 <= cp <= 0xDFFF: raise SystemExit('surrogate escape in BMP sweep at %x' % cp)
            esc.add(cp)
        else:
            esc.add({'"': 0x22, '\\': 0x5C, 'b': 8, 'f': 12, 'n': 10, 'r': 13, 't': 9, '/': 0x2F}[c]); i += 2
    else:
        raw.add(ord(text[i])); i += 1
allcp = [c for c in range(0x10000) if not 0xD800 <= c <= 0xDFFF]
missing = [c for c in allcp if c not in esc and c not in raw]
print('escaped', len(esc), 'raw', len(raw), 'missing', len(missing))
# ranges of escaped code points (surrogates counted as escaped: always written as \u pairs)
forb = esc | set(range(0xD800, 0xE000))
ranges = []
c = 0
while c < 0x10000:
    if c in forb:
        s = c
        while c + 1 < 0x10000 and (c + 1) in forb: c += 1
        ranges.append((s, c))
    c += 1
print('ranges', len(ranges))
open(sys.argv[2], 'w').write(''.join('{0x%04X,0x%04X},' % r + ('\n' if k % 8 == 7 else ' ') for k, r in enumerate(ranges)))
