#!/usr/bin/env python3
"""Comprueba que las direcciones y firmas de los mods Kuang coinciden con un GeometryDash.exe.

Uso:   python tools/verificar-exe.py "C:\\...\\Geometry Dash\\GeometryDash.exe"

Lee mod/kfe.cpp y menu/kmenu.cpp, y para cada funcion compara los bytes del exe en la direccion
indicada. Si no coinciden, busca la misma firma en todo el .text y muestra las coincidencias
cercanas (con el desplazamiento): normalmente la funcion solo se movio unos KB y basta con
actualizar la direccion. No necesita ninguna libreria externa.
"""
import re, struct, sys, os

if len(sys.argv) < 2:
    sys.exit(__doc__)
exe = open(sys.argv[1], 'rb').read()
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

e = struct.unpack_from('<I', exe, 0x3c)[0]
nsec = struct.unpack_from('<H', exe, e + 6)[0]
osz = struct.unpack_from('<H', exe, e + 20)[0]
so = e + 24 + osz
secs = []
for i in range(nsec):
    n, vs, va, rs, ro = struct.unpack_from('<8sIIII', exe, so + i * 40)
    secs.append((n.rstrip(b'\0').decode(), va, max(vs, rs), ro, rs))
text = next(s for s in secs if s[0] == '.text')
blob = exe[text[3]:text[3] + text[4]]

def at(rva, n):
    for _, va, sz, ro, rs in secs:
        if va <= rva < va + sz:
            o = ro + rva - va
            return exe[o:o + n]
    return b''

def toks(s):
    return bytes(int(x, 16) for x in re.findall(r'\b0x[0-9a-fA-F]+\b|\b0\b', s))

items = []
km = open(os.path.join(root, 'menu', 'kmenu.cpp'), encoding='utf-8').read()
for m in re.finditer(r'(?:gameFn\(base,\s*|\bfn\()(0x[0-9a-f]+),\s*\{([^}]*)\},\s*"([^"]+)"', km):
    items.append((m.group(3), int(m.group(1), 16), toks(m.group(2))))
for m in re.finditer(r'\{(0x[0-9a-f]+),\s*\{([^}]*)\},\s*reinterpret_cast<void\*>\(&\w+\),\s*reinterpret_cast<void\*\*>\(&\w+\),\s*"([^"]+)"', km):
    items.append((m.group(3), int(m.group(1), 16), toks(m.group(2))))
for m in re.finditer(r'\{(PR_\w+),\s*(0x[0-9a-f]+),\s*\{([^}]*)\}', km):
    items.append((m.group(1), int(m.group(2), 16), toks(m.group(3))))
sigs = {k: toks(v) for k, v in re.findall(r'const unsigned char (kSig\w+)\[\] = \{([^}]*)\}', km)}
for m in re.finditer(r'\{"([^"]+)",\s*(0x[0-9a-f]+),\s*(kSig\w+)', km):
    items.append((m.group(1), int(m.group(2), 16), sigs[m.group(3)]))
m = re.search(r'constexpr uintptr_t kUpdateVisRva = (0x[0-9a-f]+);', km)
if m and 'kSigUpdateVis' in sigs:
    items.append(('PlayLayer::updateVisibility', int(m.group(1), 16), sigs['kSigUpdateVis']))
kf = open(os.path.join(root, 'mod', 'kfe.cpp'), encoding='utf-8').read()
for nm, rv in re.findall(r'constexpr uintptr_t (\w+)\s*=\s*(0x[0-9a-f]+);', kf):
    s = re.search(r'constexpr unsigned char ' + nm + r'Sig\[\] = \{([^}]*)\}', kf)
    if s:
        items.append(('kfe ' + nm, int(rv, 16), toks(s.group(1))))

bad = 0
for name, rva, sig in items:
    if at(rva, len(sig)) == sig:
        print(f'OK    {name:36s} {rva:#x}')
        continue
    bad += 1
    hits = [text[1] + m.start() for m in re.finditer(re.escape(sig), blob, re.S)]
    near = sorted(hits, key=lambda h: abs(h - rva))[:4]
    desc = ', '.join(f'{h:#x} ({h - rva:+#x})' for h in near) or 'sin coincidencias (cambio el codigo de la funcion)'
    print(f'FALLA {name:36s} {rva:#x} -> candidatas: {desc}')
print('\nTodo coincide.' if not bad else f'\n{bad} direcciones por actualizar.')
