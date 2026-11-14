#!/usr/bin/env python3
"""Translate a GameMaker data.win's GLSL ES fragment shaders to Xbox 360 pixel shaders.

Modern GameMaker data.win files carry real shader code only as GLSL ES (the
HLSL9 field is just a header), so this translates each fragment shader to
HLSL and compiles it with the XDK's fxc (ps_3_0) into <out>/<name>.xps. The
XDK backend loads game:\\shaders\\<name>.xps at startup.

  python tools/xdk/translate_shaders.py <data.win> <outdir> [--fxc path] [--keep-hlsl]

Vertex shaders are not translated: the backend's fixed vertex shader feeds
v_vTexcoord (TEXCOORD0) and v_vColour (TEXCOORD1), which is all GameMaker's
default pass-through vertex shader provides.
"""
import os
import re
import struct
import subprocess
import sys

TYPES = [(r"\bvec([234])\b", r"float\1"), (r"\bivec([234])\b", r"int\1"), (r"\bbvec([234])\b", r"bool\1"),
         (r"\bmat([234])\b", r"float\1x\1"), (r"\b(lowp|mediump|highp)\b", "")]
FUNCS = {"texture2D": "tex2D", "mix": "lerp", "fract": "frac", "inversesqrt": "rsqrt",
         "dFdx": "ddx", "dFdy": "ddy", "mod": "glsl_mod"}
PRELUDE = ("#define glsl_mod(x, y) ((x) - (y) * floor((x) / (y)))\n"  # GLSL mod floors; HLSL fmod truncates
           "static float4 gl_FragColor = 0;\nstatic float4 gl_FragCoord = 0;\n")
VARYING_SEMANTICS = {"v_vTexcoord": "TEXCOORD0", "v_vColour": "TEXCOORD1"}


def read_shaders(path):
    data = open(path, "rb").read()
    pos = 8
    while pos < len(data):
        tag, length = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        if tag == b"SHDR":
            start = pos + 8
            count = struct.unpack_from("<I", data, start)[0]
            cstr = lambda o: data[o:data.index(b"\0", o)].decode("latin1")
            for ptr in struct.unpack_from("<%dI" % count, data, start + 4):
                f = struct.unpack_from("<8I", data, ptr)  # name, type, glslES vs/fs, glsl vs/fs, hlsl9 vs/fs
                yield cstr(f[0]), cstr(f[3])
            return
        pos += 8 + length


def call_args(src, open_paren):
    """Return (args, close_index) for the call whose '(' is at open_paren."""
    depth, args, last = 0, [], open_paren + 1
    for i in range(open_paren, len(src)):
        c = src[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                args.append(src[last:i])
                return args, i
        elif c == "," and depth == 1:
            args.append(src[last:i])
            last = i + 1
    raise ValueError("unbalanced parentheses")


def rewrite_calls(src, name, fn):
    out, i = [], 0
    pattern = re.compile(r"\b%s\s*\(" % name)
    while True:
        m = pattern.search(src, i)
        if not m:
            return "".join(out) + src[i:]
        args, end = call_args(src, m.end() - 1)
        args = [rewrite_calls(a, name, fn) for a in args]
        out.append(src[i:m.start()] + fn(args))
        i = end + 1


HLSL_KEYWORDS = ("point", "line", "triangle", "linear", "sample", "texture", "vector", "matrix", "string",
                 "half", "register", "pass", "compile", "shared", "snorm", "unorm", "centroid", "technique",
                 "packoffset", "inline", "extern", "volatile", "precise", "groupshared", "lineadj", "triangleadj")


def translate(glsl):
    src = glsl.replace("\r\n", "\n")
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)  # comments may hold unbalanced brackets
    src = re.sub(r"\b(%s)\b(?!\s*\()" % "|".join(HLSL_KEYWORDS), r"\1_", src)
    src = re.sub(r"^\s*(#version|#extension|precision\b).*$", "", src, flags=re.M)
    for pat, rep in TYPES:
        src = re.sub(pat, rep, src)
    varyings = re.findall(r"^\s*varying\s+(\w+)\s+(\w+)\s*;", src, flags=re.M)
    src = re.sub(r"^\s*varying\s+.*$", "", src, flags=re.M)
    src = re.sub(r"^\s*uniform\s+sampler2D\s+gm_BaseTexture\s*;", "sampler2D gm_BaseTexture : register(s0);", src, flags=re.M)
    src = re.sub(r"^(\s*)uniform\s+", r"\1", src, flags=re.M)
    src = re.sub(r"^const\s+", "static const ", src, flags=re.M)  # global const = compile-time constant
    # GLSL atan(y, x) is HLSL atan2; scalar-splat constructors need a cast in HLSL.
    src = rewrite_calls(src, "atan", lambda a: ("atan2(%s,%s)" % tuple(a)) if len(a) == 2 else "atan(%s)" % a[0])
    for n in ("float2", "float3", "float4", "int2", "int3", "int4", "bool2", "bool3", "bool4"):
        src = rewrite_calls(src, n, lambda a, n=n: "((%s)(%s))" % (n, a[0]) if len(a) == 1 else "%s(%s)" % (n, ",".join(a)))
    src = rewrite_calls(src, "texture2D", lambda a: ("tex2Dbias(%s, float4(%s, 0, %s))" % tuple(a)) if len(a) == 3
                        # No mipmaps here, so lod 0 samples the same texels; it needs no gradients,
                        # which lets texture reads sit inside dynamic loops.
                        else "tex2Dlod(%s, float4(%s, 0, 0))" % (a[0], a[1]))
    src = re.sub(r"\bdiscard\s*;", "clip(-1);", src)  # Xenon's microcode compiler has no 'discard'
    for g, h in FUNCS.items():
        src = re.sub(r"\b%s\s*\(" % g, h + "(", src)
    src = fix_matmul(src)
    src = fix_loops(src)

    m = re.search(r"\bvoid\s+main\s*\(\s*(void)?\s*\)\s*\{", src)
    if not m:
        raise ValueError("no main()")
    body_start = m.end()
    _, body_end = call_args(src.replace("{", "(").replace("}", ")"), body_start - 1)
    body = src[body_start:body_end].replace("return;", "return gl_FragColor;")
    params = ["float2 _bs_tc : TEXCOORD0", "float4 _bs_col : TEXCOORD1"]
    prologue = ""
    decls = ""
    for typ, name in varyings:
        decls += "static %s %s;\n" % (typ, name)
        if name in VARYING_SEMANTICS:
            prologue += "%s = %s;\n" % (name, "_bs_tc" if name == "v_vTexcoord" else "_bs_col")
    if "gl_FragCoord" in body:
        params.append("float2 _bs_vpos : VPOS")
        prologue += "gl_FragCoord = float4(_bs_vpos + 0.5, 0, 1);\n"
    main = "float4 main(%s) : COLOR0 {\n%s%s\nreturn gl_FragColor;\n}" % (", ".join(params), prologue, body)
    return PRELUDE + decls + src[:m.start()] + main + src[body_end + 1:]


MATRIX_NAMES = set()
IDENT = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.")


def match_back(src, close):  # index of the '(' or '[' matching src[close]
    pairs, depth = {")": "(", "]": "["}, 0
    for k in range(close, -1, -1):
        if src[k] in ")]":
            depth += 1
        elif src[k] in "([":
            depth -= 1
            if depth == 0:
                return k
    raise ValueError("unbalanced brackets")


def left_operand(src, end):
    i = end
    while i > 0 and src[i - 1].isspace():
        i -= 1
    start = i
    while start > 0 and (src[start - 1] in IDENT or src[start - 1] in ")]"):
        start = match_back(src, start - 1) if src[start - 1] in ")]" else start - 1
    return start, i


def right_operand(src, begin):
    i = begin
    while src[i].isspace():
        i += 1
    end = i
    while end < len(src) and (src[end] in IDENT or src[end] in "(["):
        end = (call_args(src, end)[1] if src[end] == "(" else src.index("]", end)) + 1 if src[end] in "([" else end + 1
    return i, end


def fix_matmul(src):
    # GLSL matrices are column-major and HLSL constructors fill rows with the same
    # arguments, so GLSL v * M == mul(M, v) and M * v == mul(v, M).
    pos = 0
    while True:  # inline constructors: floatNxN(...) * v  and  v * floatNxN(...)
        m = re.compile(r"\bfloat([234])x\1\s*\(").search(src, pos)
        if not m:
            break
        _, close = call_args(src, m.end() - 1)
        ctor = src[m.start():close + 1]
        after = re.match(r"\s*\*(?!=)", src[close + 1:])
        before = re.search(r"\*\s*$", src[:m.start()])
        if after:
            s, e = right_operand(src, close + 1 + after.end())
            new = "mul(%s, %s)" % (src[s:e], ctor)
            src = src[:m.start()] + new + src[e:]
            pos = m.start() + len(new)
        elif before:
            s, e = left_operand(src, before.start())
            new = "mul(%s, %s)" % (ctor, src[s:e])
            src = src[:s] + new + src[close + 1:]
            pos = s + len(new)
        else:
            pos = close + 1
    if not MATRIX_NAMES:
        return src
    names = "|".join(sorted(MATRIX_NAMES, key=len, reverse=True))
    while True:
        m = re.search(r"(\*=|\*)\s*(%s)\b(?!\s*[\[(])" % names, src)  # ... * M
        if m:
            s, e = left_operand(src, m.start())
            left = src[s:e]
            repl = ("%s = mul(%s, %s)" if m.group(1) == "*=" else "%s(mul(%s, %s))") % (
                (left, m.group(2), left) if m.group(1) == "*=" else ("", m.group(2), left))
            src = src[:s] + repl + src[m.end():]
            continue
        m = re.search(r"\b(%s)\s*\*(?!=)" % names, src)  # M * ...
        if m:
            s, e = right_operand(src, m.end())
            src = src[:m.start()] + "(mul(%s, %s))" % (src[s:e], m.group(1)) + src[e:]
            continue
        return src


def fix_loops(src):
    # Xenon's compiler must prove termination; cap data-dependent loops at 255 iterations (the ps_3_0 loop counter limit).
    out, i, n = [], 0, 0
    for m in re.finditer(r"\bfor\s*\(", src):
        args, close = call_args(src.replace(";", ","), m.end() - 1)
        if len(args) != 3 or re.fullmatch(r"\s*\w+\s*<=?\s*[\d.]+\s*", args[1]):
            continue  # constant bound: fine as is
        b = close + 1
        while src[b].isspace():
            b += 1
        if src[b] != "{" or re.search(r"\bcontinue\b", src[b:]):
            continue  # shortcut: only brace bodies without 'continue' are rewritten
        _, body_end = call_args(src.replace("{", "(").replace("}", ")"), b)
        # Xenon only proves termination for int-counted loops: run the original
        # loop as an early-exit body of a capped one.
        guard = "_bs_n%d" % n
        n += 1
        out.append(src[i:m.start()] + "{ %s; [loop] for (int %s = 0; %s < 255; %s++) { if (!(%s)) break; %s %s; } }" % (
            args[0], guard, guard, guard, args[1], src[b + 1:body_end], args[2]))
        i = body_end + 1
    return "".join(out) + src[i:]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    datawin, outdir = sys.argv[1], sys.argv[2]
    fxc = os.path.join(os.environ.get("XEDK", ""), "bin", "win32", "fxc.exe")
    if "--fxc" in sys.argv:
        fxc = sys.argv[sys.argv.index("--fxc") + 1]
    os.makedirs(outdir, exist_ok=True)
    ok, failed = 0, []
    for name, frag in read_shaders(datawin):
        MATRIX_NAMES.clear()
        MATRIX_NAMES.update(re.findall(r"\bmat[234]\s+(\w+)", frag))
        hlsl_path = os.path.join(outdir, name + ".hlsl")
        try:
            open(hlsl_path, "w").write(translate(frag))
        except ValueError as e:
            failed.append((name, str(e)))
            continue
        result = subprocess.run([fxc, "/nologo", "/T", "ps_3_0", "/E", "main", "/Fo",
                                 os.path.join(outdir, name + ".xps"), hlsl_path],
                                capture_output=True, text=True)
        if result.returncode == 0:
            ok += 1
            if "--keep-hlsl" not in sys.argv:
                os.remove(hlsl_path)
        else:
            err = (result.stdout + result.stderr).strip().splitlines()
            failed.append((name, next((l for l in err if "error" in l), err[-1] if err else "?")))
    print("%d/%d shaders compiled" % (ok, ok + len(failed)))
    for name, err in failed:
        print("  FAIL %s: %s" % (name, err[:200]))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
