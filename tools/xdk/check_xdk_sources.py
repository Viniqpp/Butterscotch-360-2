#!/usr/bin/env python3
"""Host-side consistency check for the Xbox 360 XDK target.

Runs anywhere Python 3 runs; needs no XDK. It does NOT prove the XDK build
compiles. It catches the regressions that broke this target before:
  * vcxproj source entries that no longer resolve to files,
  * Debug/Release XDK library lists drifting apart,
  * platform vtable members the XDK adapters leave NULL although the core
    calls them without a NULL check (the renderer once crashed this way).

Usage: python3 tools/xdk/check_xdk_sources.py [repo_root]
"""
import glob
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", ".."))
NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}

# Renderer members the core only calls behind a NULL check. Anything else the
# D3D9 backend leaves unassigned is a crash waiting to happen.
RENDERER_OPTIONAL = {
    "clearDepth", "drawTile", "drawTiledPart", "drawVertexBuffer", "gpuSetFog",
    "setDepthState", "shaderSetUniformFArray", "surfaceSetPixels", "surfaceUploadPixels",
}

DEBUG_LIBS = {"d3d9d.lib", "d3dx9d.lib", "xgraphicsd.lib", "xapilibd.lib", "xaudiod2.lib", "xmcored.lib", "xboxkrnl.lib", "xbdm.lib"}
RELEASE_LIBS = {"d3d9.lib", "d3dx9.lib", "xgraphics.lib", "xapilib.lib", "xaudio2.lib", "xmcore.lib", "xboxkrnl.lib"}

errors = []


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def vtable_members(header, struct_name):
    text = read(header)
    end = text.index("} %s;" % struct_name)
    start = text.rindex("typedef struct", 0, end)
    return set(re.findall(r"\(\s*\*\s*(\w+)\s*\)", text[start:end]))


def assigned(source, var):
    return set(re.findall(r"\b%s\.(\w+)\s*=" % re.escape(var), read(source)))


def check_vtable(label, header, struct_name, source, var, optional=frozenset()):
    members = vtable_members(header, struct_name)
    have = assigned(source, var)
    unknown = have - members
    missing = members - have - set(optional)
    if unknown:
        errors.append("%s: assigns members not in %s: %s" % (label, struct_name, sorted(unknown)))
    if missing:
        errors.append("%s: leaves %d %s member(s) NULL that the core may call unguarded: %s"
                      % (label, len(missing), struct_name, sorted(missing)))
    print("%-10s %2d/%2d members assigned (%d optional left NULL)"
          % (label, len(have & members), len(members), len(members - have)))


def check_quoted_includes(path, tree, base, sources):
    # Every unconditional #include "x" in a compiled file must resolve through the
    # project's include paths (src\video was once missing). Includes inside any
    # #if block are skipped: they are usually other platforms' headers.
    configs = {}
    for group in tree.iterfind(".//m:ItemDefinitionGroup", NS):
        dirs = group.find("m:ClCompile/m:AdditionalIncludeDirectories", NS)
        if dirs is not None:
            configs[group.get("Condition") or ""] = [
                os.path.join(base, d.replace("\\\\", "\\").replace("\\", os.sep))
                for d in dirs.text.split(";") if d and not d.startswith("%(")]
    for cond, dirs in configs.items():
        for d in dirs:
            if not os.path.isdir(d):
                errors.append("%s: include directory '%s' does not exist" % (path, os.path.relpath(d, ROOT)))
        for src in sources:
            depth = 0
            with open(src, encoding="utf-8", errors="replace") as f:
                for n, line in enumerate(f, 1):
                    s = line.strip()
                    if re.match(r"#\s*if", s):
                        depth += 1
                    elif re.match(r"#\s*endif", s):
                        depth -= 1
                    m = re.match(r'#\s*include\s+"([^"]+)"', s)
                    if depth == 0 and m:
                        name = m.group(1).replace("/", os.sep)
                        roots = [os.path.dirname(src)] + dirs
                        if not any(os.path.isfile(os.path.join(r, name)) for r in roots):
                            errors.append('%s (%s): %s:%d #include "%s" not found on the include path'
                                          % (path, "Debug" if "Debug|" in cond else "Release",
                                             os.path.relpath(src, ROOT), n, m.group(1)))


def check_vcxproj(path):
    tree = ET.parse(os.path.join(ROOT, path))
    base = os.path.dirname(os.path.join(ROOT, path))  # MSBuild items are project-relative
    count = 0
    compiled = []
    for kind in ("ClCompile", "ClInclude"):
        for node in tree.iterfind(".//m:%s" % kind, NS):
            pattern = node.get("Include")
            if not pattern:
                continue
            excludes = {os.path.normcase(os.path.join(base, e.replace("\\", os.sep)))
                        for e in (node.get("Exclude") or "").split(";") if e}
            matches = [m for m in glob.glob(os.path.join(base, pattern.replace("\\", os.sep)))
                       if os.path.normcase(m) not in excludes]
            if not matches:
                errors.append("%s: %s '%s' matches no files" % (path, kind, pattern))
            count += len(matches)
            if kind == "ClCompile":
                compiled += matches
    check_quoted_includes(path, tree, base, compiled)
    libs = {}
    for group in tree.iterfind(".//m:ItemDefinitionGroup", NS):
        cond = group.get("Condition") or ""
        deps = group.find("m:Link/m:AdditionalDependencies", NS)
        if deps is not None:
            libs["Debug" if "Debug|" in cond else "Release"] = {
                d for d in deps.text.split(";") if d and not d.startswith("%(")}
    for cfg, expected in (("Debug", DEBUG_LIBS), ("Release", RELEASE_LIBS)):
        if libs.get(cfg) != expected:
            errors.append("%s: %s libraries %s != expected %s" % (path, cfg, sorted(libs.get(cfg, ())), sorted(expected)))
    print("vcxproj    %-35s %d files resolved, Debug/Release XDK libs checked" % (path, count))


def check_no_c99_restrict():
    # `restrict` is not a C++ keyword and these .c files are compiled with /TP.
    for path in glob.glob(os.path.join(ROOT, "src", "**", "*.c"), recursive=True):
        if os.sep + "xbox360" + os.sep in path or os.sep + "ps2" + os.sep in path:
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                code = line.split("//", 1)[0]
                if re.search(r"\brestrict\b", code):
                    errors.append("%s:%d uses C99 'restrict'; use __restrict for the XDK /TP build"
                                  % (os.path.relpath(path, ROOT), n))


check_vtable("renderer", "src/renderer.h", "RendererVtable",
             "src/xbox360-xdk/d3d9_renderer.cpp", "d3d9RendererVtable", RENDERER_OPTIONAL)
check_vtable("audio", "src/audio_system.h", "AudioSystemVtable",
             "src/xbox360-xdk/xaudio2_audio.cpp", "xdkAudioVtable")
check_vtable("filesystem", "src/file_system.h", "FileSystemVtable",
             "src/xbox360-xdk/xdk_file_system.cpp", "xdkFileSystemVtable")
check_vcxproj("Butterscotch360.vcxproj")
check_vcxproj("tools/xdk/smoke/XdkSmoke.vcxproj")
check_no_c99_restrict()

if errors:
    print("\nFAILED:")
    for e in errors:
        print("  - " + e)
    sys.exit(1)
print("\nOK (source-level only: this is not an XDK compile)")
