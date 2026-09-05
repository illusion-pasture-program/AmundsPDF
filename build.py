#!/usr/bin/env python3
"""
AmundsPDF build script.

    python build.py              release build  -> build/AmundsPDF.exe
    python build.py --debug      symbols, console output, no optimisation
    python build.py --run F.pdf  build, then open F.pdf
    python build.py --pack       release build, then UPX-compress it
    python build.py --clean      remove build/

Requires MinGW-w64 GCC on PATH. No other tooling, no libraries.
"""

import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
OUT = os.path.join(ROOT, "build")

SOURCES = [
    "main.c",
    "pdf_parser.c",
    "pdf_render.c",
    "pdf_raster.c",
    "pdf_fonts.c",
    "pdf_glyph_cff.c",
    "pdf_glyph_type1.c",
    "pdf_glyph_tt_stub.c",
    "pdf_glyph_render.c",
    "pdf_inflate.c",
    "pdf_crypt.c",
]

# All OS DLLs -- there are no third-party dependencies.
#   ole32 + windowscodecs  WIC, used only to decode JPEG and friends
#   msimg32               AlphaBlend / TransparentBlt
LIBS = [
    "-lgdi32", "-luser32", "-lshell32", "-lcomdlg32",
    "-lole32", "-lwindowscodecs", "-lmsimg32",
]

# pdf_profile.h uses `bool` without including <stdbool.h>, which only compiles
# where bool is a keyword. Pin the standard rather than relying on whatever
# the installed GCC happens to default to; this needs GCC 14 or newer.
COMMON = [
    "-std=gnu23",
    "-municode",
    "-Wall",
    "-Wno-unused-parameter",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-fno-ident",
]

# Note the absence of -fdata-sections. g_encoding_cache in pdf_glyph_render.c
# is a ~1 MB zero-filled static; with -fdata-sections it stops being BSS and
# gets written into the executable as literal zeros, taking the binary from
# 279 KB to 1.4 MB. It is invisible if you then UPX-pack (zeros compress to
# nothing), which is exactly how it went unnoticed. -ffunction-sections on its
# own was measured to save nothing here, so neither flag is used.
RELEASE = COMMON + ["-O2", "-DNDEBUG"]
DEBUG = COMMON + ["-O0", "-g", "-DDEBUG=1"]

LINK_RELEASE = ["-municode", "-mwindows", "-Wl,--gc-sections", "-s"]
LINK_DEBUG = ["-municode", "-mconsole", "-Wl,--gc-sections"]


def run(cmd):
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        sys.exit(r.returncode)


def clean():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    print("cleaned")


def build(debug=False):
    os.makedirs(OUT, exist_ok=True)
    flags = DEBUG if debug else RELEASE
    link = LINK_DEBUG if debug else LINK_RELEASE
    exe = os.path.join(OUT, "AmundsPDF.exe")

    t0 = time.time()
    newest_h = max(
        (os.path.getmtime(os.path.join(SRC, h))
         for h in os.listdir(SRC) if h.endswith(".h")),
        default=0,
    )

    objs = []
    for s in SOURCES:
        src = os.path.join(SRC, s)
        obj = os.path.join(OUT, s[:-2] + ".o")
        if not os.path.exists(src):
            sys.exit("missing source: " + src)
        if (os.path.exists(obj)
                and os.path.getmtime(obj) > os.path.getmtime(src)
                and os.path.getmtime(obj) > newest_h):
            objs.append(obj)
            continue
        print("  cc  " + s)
        run(["gcc"] + flags + ["-c", src, "-o", obj])
        objs.append(obj)

    rc = os.path.join(SRC, "resource.rc")
    if os.path.exists(rc):
        res = os.path.join(OUT, "resource.o")
        print("  rc  resource.rc")
        run(["windres", "-I", SRC, rc, "-o", res])
        objs.append(res)

    print("  ld  AmundsPDF.exe")
    run(["gcc"] + objs + link + LIBS + ["-o", exe])

    print("\n%s  %.1f KB  (%.2fs)"
          % (exe, os.path.getsize(exe) / 1024.0, time.time() - t0))
    return exe


def pack(exe):
    """UPX-compress in place. Roughly halves the binary.

    Not used for the published releases: packers set off antivirus
    heuristics, which is a bad trade for something people download and run.
    """
    if not shutil.which("upx"):
        sys.exit("--pack needs UPX on PATH (https://upx.github.io/)")
    before = os.path.getsize(exe)
    run(["upx", "--best", "--lzma", exe])
    after = os.path.getsize(exe)
    print("packed  %.1f KB -> %.1f KB  (%.0f%%)"
          % (before / 1024.0, after / 1024.0, 100.0 * after / before))


def main():
    args = sys.argv[1:]
    if "--clean" in args:
        clean()
        return

    exe = build("--debug" in args)

    if "--pack" in args:
        pack(exe)

    if "--run" in args:
        i = args.index("--run")
        target = args[i + 1] if i + 1 < len(args) else ""
        subprocess.Popen([exe] + ([target] if target else []))


if __name__ == "__main__":
    main()
