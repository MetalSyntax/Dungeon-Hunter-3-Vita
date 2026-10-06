#!/usr/bin/env python3
"""
dh3_pak.py -- lector del pak propio de DH3 (el .obb y los .pak internos).

Formato reconstruido de libDH3.so (CCustomPakReader::scanLocalHeader 0x67e128,
DECODE_XOR32 0x67fbb8, getRand 0x67f8c8):
  header  : 4 x u32 LE  = (?, namesEnd, namesStart, count)
  entries : count x (u32 offset, u32 size, u32 nameOff, u32 ?)   @ 0x10
  names   : [namesStart, namesEnd) -- nombre i = names[nameOff_i : nameOff_{i+1}-1]
            ofuscado con DECODE_XOR32(key=0x3857a), semilla reiniciada por nombre
  Si el flag "ignoreCase" del lector esta activo, los nombres se pasan a minusculas.

Uso:
  dh3_pak.py list    <pak>              # offset size nombre
  dh3_pak.py extract <pak> <regex> <dir> # extrae los que matcheen
"""
import os, re, struct, sys

KEY = 0x3857A


class Rand:
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def __call__(self, n):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        r = self.s >> 16
        return r % n if n else r


def _rot(v, sh, d, bits):
    m = (1 << bits) - 1
    if d:
        v = (v << (bits - sh)) | (v >> sh)   # ror sh
    else:
        v = (v >> (bits - sh)) | (v << sh)   # rol sh
    return v & m


def decode_xor32(buf, key=KEY):
    rnd = Rand(key)
    out = bytearray(buf)
    n = len(buf)
    done = 0
    if n > 3:
        words = (n - 4) // 4 + 1
        for i in range(words):
            w = struct.unpack_from("<I", buf, i * 4)[0]
            d = rnd(2)
            sh = rnd(32)
            w = _rot(w, sh, d, 32) if sh else w
            a, b, c, e = rnd(256), rnd(256), rnd(256), rnd(256)
            k = (a << 24) | (b << 16) | (c << 8) | e
            struct.pack_into("<I", out, i * 4, w ^ k)
        done = words * 4
    rem = n - done
    d = rnd(2)
    if rem == 1:
        sh = rnd(8)
        v = _rot(buf[done], sh, d, 8) ^ rnd(256)
        out[done] = v & 0xFF
    elif rem == 2:
        v = buf[done] | (buf[done + 1] << 8)
        sh = rnd(16)
        v = _rot(v, sh, d, 16)
        a, b = rnd(256), rnd(256)
        v ^= (a << 8) | b
        out[done:done + 2] = struct.pack("<H", v & 0xFFFF)
    elif rem == 3:
        v = buf[done] | (buf[done + 1] << 8) | (buf[done + 2] << 16)
        sh = rnd(24)
        v = _rot(v, sh, d, 24)
        a, b, c = rnd(256), rnd(256), rnd(256)
        v ^= (a << 16) | (b << 8) | c
        out[done:done + 3] = bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF])
    return bytes(out)


def read_index(f):
    f.seek(0)
    _, names_end, names_start, count = struct.unpack("<4I", f.read(16))
    ents = [struct.unpack("<4I", f.read(16)) for _ in range(count)]
    f.seek(names_start)
    names = f.read(names_end - names_start)
    out = []
    for i, (off, size, noff, extra) in enumerate(ents):
        nend = ents[i + 1][2] if i + 1 < count else len(names)
        raw = names[noff:nend - 1]
        out.append((off, size, decode_xor32(raw).decode("latin-1"), extra))
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, pak = sys.argv[1], sys.argv[2]
    with open(pak, "rb") as f:
        idx = read_index(f)
        if cmd == "list":
            for off, size, name, extra in idx:
                print("%10x %10d %s%s" % (off, size, name, "" if not extra else "  [%x]" % extra))
        elif cmd == "extract":
            rx, dst = re.compile(sys.argv[3]), sys.argv[4]
            for off, size, name, _ in idx:
                if rx.search(name):
                    p = os.path.join(dst, name.lstrip("/"))
                    os.makedirs(os.path.dirname(p) or dst, exist_ok=True)
                    f.seek(off)
                    open(p, "wb").write(f.read(size))
                    print(p)
        else:
            sys.exit(__doc__)


if __name__ == "__main__":
    main()
