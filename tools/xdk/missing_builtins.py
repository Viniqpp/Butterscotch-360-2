# Lists GameMaker functions a data.win calls that the runner does not implement,
# with how many call sites use each. Usage: python tools/xdk/missing_builtins.py a.win [b.win ...]
import glob, os, re, struct, sys, collections

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src")
builtins = set()
for path in glob.glob(os.path.join(root, "*.c")):
    builtins |= set(re.findall(r'VM_registerBuiltin\(\s*\w+\s*,\s*"([^"]+)"', open(path, encoding="utf-8", errors="replace").read()))

total = collections.Counter()
for win in sys.argv[1:]:
    d = open(win, "rb").read()
    chunks, i = {}, 8
    while i < len(d):
        chunks[d[i:i + 4].decode()] = i + 8
        i += 8 + struct.unpack_from("<I", d, i + 4)[0]
    cstr = lambda p: d[p:d.index(b"\0", p)].decode("utf8", "replace")
    co = chunks["CODE"]
    codes = {cstr(struct.unpack_from("<I", d, p)[0]) for p in
             struct.unpack_from("<%dI" % struct.unpack_from("<I", d, co)[0], d, co + 4)}
    fo = chunks["FUNC"]
    missing = collections.Counter()
    for k in range(struct.unpack_from("<I", d, fo)[0]):
        name_ptr, occurrences, _ = struct.unpack_from("<III", d, fo + 4 + k * 12)
        name = cstr(name_ptr)
        if occurrences and name not in builtins and name not in codes and not name.startswith(("gml_Script_", "@@")):
            missing[name] += occurrences
    total.update(missing)
    print("== %s: %d missing functions" % (win, len(missing)))
    print(", ".join("%s(%d)" % kv for kv in missing.most_common()))
if len(sys.argv) > 2:
    print("== all games:", ", ".join("%s(%d)" % kv for kv in total.most_common(60)))
