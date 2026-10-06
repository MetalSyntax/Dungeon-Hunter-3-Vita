#!/usr/bin/env python3
"""
so_annotate.py -- desensamblado anotado de libDH3.so (sin Ghidra).

libDH3.so trae .symtab completo (~160K simbolos C++) pero DWARF solo de
libgcc, asi que el camino rapido es objdump + resolver a mano:
  * literales PC-relativos  (ldr rX,[pc,#n] ; add rX,pc,rX)  -> "string" / <simbolo>
  * llamadas JNIEnv         (ldr rY,[rZ,#off] ; blx rY)       -> ;; JNI?<Nombre>
    (heuristica: el offset coincide con la tabla JNINativeInterface; con
    JavaVM* los nombres salen corridos -- 16=AttachCurrentThread, 24=GetEnv)

Uso:
  so_annotate.py fn  <simbolo_mangled>     # una funcion, anotada
  so_annotate.py jni                        # barrido: todos los FindClass /
                                            # Get*MethodID / Get*FieldID con
                                            # args literales (tabla FalsoJNI real)
  so_annotate.py callers <simbolo_mangled>  # funciones que hacen bl/b al simbolo

Requiere: arm-vita-eabi-objdump/nm (VitaSDK), pyelftools. Cachea el
desensamblado completo en build/so_annotate/ (gitignored).
"""
import os, re, subprocess, sys
from elftools.elf.elffile import ELFFile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SO = os.path.join(ROOT, "dungeonhunter3_extract/lib/armeabi-v7a/libDH3.so")
CACHE = os.path.join(ROOT, "build/so_annotate")
VITASDK = os.environ.get("VITASDK", os.path.expanduser("~/vitasdk"))
OBJDUMP = os.path.join(VITASDK, "bin/arm-vita-eabi-objdump")
NM = os.path.join(VITASDK, "bin/arm-vita-eabi-nm")

JNI = """NULL NULL NULL NULL GetVersion DefineClass FindClass FromReflectedMethod FromReflectedField ToReflectedMethod GetSuperclass IsAssignableFrom ToReflectedField Throw ThrowNew ExceptionOccurred ExceptionDescribe ExceptionClear FatalError PushLocalFrame PopLocalFrame NewGlobalRef DeleteGlobalRef DeleteLocalRef IsSameObject NewLocalRef EnsureLocalCapacity AllocObject NewObject NewObjectV NewObjectA GetObjectClass IsInstanceOf GetMethodID CallObjectMethod CallObjectMethodV CallObjectMethodA CallBooleanMethod CallBooleanMethodV CallBooleanMethodA CallByteMethod CallByteMethodV CallByteMethodA CallCharMethod CallCharMethodV CallCharMethodA CallShortMethod CallShortMethodV CallShortMethodA CallIntMethod CallIntMethodV CallIntMethodA CallLongMethod CallLongMethodV CallLongMethodA CallFloatMethod CallFloatMethodV CallFloatMethodA CallDoubleMethod CallDoubleMethodV CallDoubleMethodA CallVoidMethod CallVoidMethodV CallVoidMethodA CallNonvirtualObjectMethod CallNonvirtualObjectMethodV CallNonvirtualObjectMethodA CallNonvirtualBooleanMethod CallNonvirtualBooleanMethodV CallNonvirtualBooleanMethodA CallNonvirtualByteMethod CallNonvirtualByteMethodV CallNonvirtualByteMethodA CallNonvirtualCharMethod CallNonvirtualCharMethodV CallNonvirtualCharMethodA CallNonvirtualShortMethod CallNonvirtualShortMethodV CallNonvirtualShortMethodA CallNonvirtualIntMethod CallNonvirtualIntMethodV CallNonvirtualIntMethodA CallNonvirtualLongMethod CallNonvirtualLongMethodV CallNonvirtualLongMethodA CallNonvirtualFloatMethod CallNonvirtualFloatMethodV CallNonvirtualFloatMethodA CallNonvirtualDoubleMethod CallNonvirtualDoubleMethodV CallNonvirtualDoubleMethodA CallNonvirtualVoidMethod CallNonvirtualVoidMethodV CallNonvirtualVoidMethodA GetFieldID GetObjectField GetBooleanField GetByteField GetCharField GetShortField GetIntField GetLongField GetFloatField GetDoubleField SetObjectField SetBooleanField SetByteField SetCharField SetShortField SetIntField SetLongField SetFloatField SetDoubleField GetStaticMethodID CallStaticObjectMethod CallStaticObjectMethodV CallStaticObjectMethodA CallStaticBooleanMethod CallStaticBooleanMethodV CallStaticBooleanMethodA CallStaticByteMethod CallStaticByteMethodV CallStaticByteMethodA CallStaticCharMethod CallStaticCharMethodV CallStaticCharMethodA CallStaticShortMethod CallStaticShortMethodV CallStaticShortMethodA CallStaticIntMethod CallStaticIntMethodV CallStaticIntMethodA CallStaticLongMethod CallStaticLongMethodV CallStaticLongMethodA CallStaticFloatMethod CallStaticFloatMethodV CallStaticFloatMethodA CallStaticDoubleMethod CallStaticDoubleMethodV CallStaticDoubleMethodA CallStaticVoidMethod CallStaticVoidMethodV CallStaticVoidMethodA GetStaticFieldID GetStaticObjectField GetStaticBooleanField GetStaticByteField GetStaticCharField GetStaticShortField GetStaticIntField GetStaticLongField GetStaticFloatField GetStaticDoubleField SetStaticObjectField SetStaticBooleanField SetStaticByteField SetStaticCharField SetStaticShortField SetStaticIntField SetStaticLongField SetStaticFloatField SetStaticDoubleField NewString GetStringLength GetStringChars ReleaseStringChars NewStringUTF GetStringUTFLength GetStringUTFChars ReleaseStringUTFChars GetArrayLength NewObjectArray GetObjectArrayElement SetObjectArrayElement NewBooleanArray NewByteArray NewCharArray NewShortArray NewIntArray NewLongArray NewFloatArray NewDoubleArray GetBooleanArrayElements GetByteArrayElements GetCharArrayElements GetShortArrayElements GetIntArrayElements GetLongArrayElements GetFloatArrayElements GetDoubleArrayElements ReleaseBooleanArrayElements ReleaseByteArrayElements ReleaseCharArrayElements ReleaseShortArrayElements ReleaseIntArrayElements ReleaseLongArrayElements ReleaseFloatArrayElements ReleaseDoubleArrayElements GetBooleanArrayRegion GetByteArrayRegion GetCharArrayRegion GetShortArrayRegion GetIntArrayRegion GetLongArrayRegion GetFloatArrayRegion GetDoubleArrayRegion SetBooleanArrayRegion SetByteArrayRegion SetCharArrayRegion SetShortArrayRegion SetIntArrayRegion SetLongArrayRegion SetFloatArrayRegion SetDoubleArrayRegion RegisterNatives UnregisterNatives MonitorEnter MonitorExit GetJavaVM GetStringRegion GetStringUTFRegion GetPrimitiveArrayCritical ReleasePrimitiveArrayCritical GetStringCritical ReleaseStringCritical NewWeakGlobalRef DeleteWeakGlobalRef ExceptionCheck NewDirectByteBuffer GetDirectBufferAddress GetDirectBufferCapacity GetObjectRefType""".split()
LOOKUPS = {"FindClass", "GetStaticMethodID", "GetMethodID", "GetFieldID",
           "GetStaticFieldID", "RegisterNatives"}

_f = open(SO, "rb")
_elf = ELFFile(_f)
_segs = [(s["p_vaddr"], s["p_filesz"], s["p_offset"])
         for s in _elf.iter_segments() if s["p_type"] == "PT_LOAD"]


def rd(a, n):
    for v, sz, o in _segs:
        if v <= a < v + sz:
            _f.seek(o + a - v)
            return _f.read(n)
    return None


def word(a):
    b = rd(a, 4)
    return int.from_bytes(b, "little") if b else None


def cstr(a):
    b = rd(a, 200)
    if not b:
        return None
    s = b.split(b"\0")[0]
    return s.decode() if len(s) >= 2 and all(32 <= c < 127 for c in s) else None


def symtab():
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, "syms.txt")
    if not os.path.exists(path):
        with open(path, "w") as o:
            subprocess.run([NM, "-S", "--defined-only", SO], stdout=o, check=True)
    syms, by_addr = {}, {}
    for l in open(path):
        p = l.split()
        if len(p) == 4:
            a = int(p[0], 16)
            syms[p[3]] = (a, int(p[1], 16))
            by_addr[a & ~1] = p[3]
    return syms, by_addr


def disasm(start=None, stop=None):
    if start is None:
        path = os.path.join(CACHE, "full.s")
        if not os.path.exists(path):
            with open(path, "w") as o:
                subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", "-j", ".text", SO],
                               stdout=o, check=True)
        return open(path)
    out = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn",
                          "--start-address=%d" % start, "--stop-address=%d" % stop, SO],
                         capture_output=True, text=True, check=True).stdout
    return out.splitlines(True)


def annotate(lines, by_addr, sweep=None):
    """Yields annotated lines; with sweep=list, appends JNI lookups instead."""
    func, lit, regs, env = "?", {}, {}, {}
    for line in lines:
        if line.endswith(">:\n"):
            func, lit, regs, env = line.split("<", 1)[1][:-3], {}, {}, {}
            if sweep is None:
                yield line.rstrip()
            continue
        m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*([^;]*)(;.*)?", line)
        out = line.rstrip()
        if not m:
            if sweep is None:
                yield out
            continue
        addr, op, args, cmt = int(m.group(1), 16), m.group(2), m.group(3).strip(), m.group(4) or ""
        note = ""
        mm = re.match(r"(\w+), \[pc, #-?\d+\]", args)
        if op == "ldr" and mm:
            ca = re.search(r"([0-9a-f]+) <", cmt)
            v = word(int(ca.group(1), 16)) if ca else None
            lit[mm.group(1)] = v
            regs.pop(mm.group(1), None)
            if v is not None:
                s = cstr(v)
                note = '   @"%s"' % s if s else ("   @<%s>" % by_addr[v & ~1] if (v & ~1) in by_addr else "")
        elif op == "add" and re.match(r"(\w+), pc, (\w+)$", args):
            d, r = re.match(r"(\w+), pc, (\w+)$", args).groups()
            if lit.get(r) is not None:
                v = (lit[r] + addr + 8) & 0xFFFFFFFF
                s = cstr(v)
                if s:
                    regs[d] = s
                    note = '   ="%s"' % s
                else:
                    regs.pop(d, None)
                    note = "   =<%s>" % by_addr[v & ~1] if (v & ~1) in by_addr else "   =0x%x" % v
        elif op == "mov" and re.match(r"(\w+), (\w+)$", args):
            d, r = re.match(r"(\w+), (\w+)$", args).groups()
            if r in regs:
                regs[d] = regs[r]
            else:
                regs.pop(d, None)
            env.pop(d, None)
        elif op == "ldr" and re.match(r"(\w+), \[(\w+), #(\d+)\]", args):
            d, _, off = re.match(r"(\w+), \[(\w+), #(\d+)\]", args).groups()
            off = int(off)
            if off % 4 == 0 and 16 <= off and off // 4 < len(JNI):
                env[d] = JNI[off // 4]
            regs.pop(d, None)
        elif op == "blx" and re.match(r"(\w+)$", args) and args in env:
            n = env[args]
            note = "   ;; JNI?%s" % n
            if sweep is not None and n in LOOKUPS:
                sweep.append((addr, func, n, regs.get("r1", ""), regs.get("r2", ""), regs.get("r3", "")))
            for r in ("r0", "r1", "r2", "r3", "ip", "lr"):
                regs.pop(r, None)
        elif op in ("bl", "blx"):
            for r in ("r0", "r1", "r2", "r3", "ip", "lr"):
                regs.pop(r, None)
        if sweep is None:
            yield out + note


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    syms, by_addr = symtab()
    cmd = sys.argv[1]
    if cmd == "fn":
        a, sz = syms[sys.argv[2]]
        a &= ~1
        for l in annotate(disasm(a, a + sz), by_addr):
            print(l)
    elif cmd == "jni":
        hits = []
        for _ in annotate(disasm(), by_addr, sweep=hits):
            pass
        seen = set()
        for addr, func, n, r1, r2, r3 in hits:
            key = (n, r1 if n == "FindClass" else r2, r3)
            if (n == "FindClass" and not r1) or (n != "FindClass" and not r2) or key in seen:
                continue
            seen.add(key)
            print("%x\t%s\t%s\t%s\t%s" % (addr, n, r1 if n == "FindClass" else r2, r3, func))
    elif cmd == "callers":
        tgt = "<%s>" % sys.argv[2]
        func, found = "?", set()
        for line in disasm():
            if line.endswith(">:\n"):
                func = line.split("<", 1)[1][:-3]
            elif tgt in line and re.search(r"\s(bl|b|blx)\s", line):
                found.add(func)
        print("\n".join(sorted(found)))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
