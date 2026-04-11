"""PlatformIO pre-build script: patch ESP32-audioI2S Icy-MetaData:2.

The library's httpPrint() (used for HTTP redirects) sends both Icy-MetaData:1
and Icy-MetaData:2 headers.  Some CDNs (e.g. Amperwave/Audacy) disable ICY
metadata entirely when they see the :2 header, so StreamTitle never arrives.
connecttohost() already has the :2 line commented out — this script does the
same for httpPrint().
"""
Import("env")  # noqa: F821 — PlatformIO/SCons built-in

import os

MARKER = "// patched: Icy-MetaData:2 breaks Amperwave ICY"


def _patch(env):
    libdeps = os.path.join(
        env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV")
    )
    audio_cpp = None
    for root, _dirs, files in os.walk(libdeps):
        if "Audio.cpp" in files and "ESP32-audioI2S" in root:
            audio_cpp = os.path.join(root, "Audio.cpp")
            break

    if not audio_cpp:
        print("  [patch] Audio.cpp not found — skipping Icy-MetaData:2 patch")
        return

    with open(audio_cpp, "r") as f:
        lines = f.readlines()

    patched = False
    for i, line in enumerate(lines):
        # Match the uncommented Icy-MetaData:2 append (in httpPrint).
        # The one in connecttohost is already commented out in upstream.
        stripped = line.strip()
        if (
            stripped == 'rqh.append("Icy-MetaData:2\\r\\n");'
            and MARKER not in line
        ):
            indent = line[: len(line) - len(line.lstrip())]
            lines[i] = f"{indent}// {stripped} {MARKER}\n"
            patched = True
            break  # only one uncommented instance

    if patched:
        with open(audio_cpp, "w") as f:
            f.writelines(lines)
        print("  [patch] Audio.cpp: commented out Icy-MetaData:2 in httpPrint()")
    elif any(MARKER in l for l in lines):
        print("  [patch] Audio.cpp: already patched (Icy-MetaData:2)")
    else:
        print("  [patch] Audio.cpp: Icy-MetaData:2 line not found")


_patch(env)  # noqa: F821
