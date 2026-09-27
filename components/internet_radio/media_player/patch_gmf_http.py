"""Pre-build script: patch ESP-GMF HTTP IO for ICY header extraction.

esp_gmf_io_http.c's internal _http_event_handle only processes
Content-Encoding headers.  This patch adds extraction of icy-metaint
and icy-br response headers into global volatiles that
internet_radio.cpp reads on the first ON_RESPONSE callback.

Idempotent: checks for and verifies each patch component individually.

Operates in two modes:
1. PlatformIO pre: script — injects a CMake hook that runs after project(),
   once the IDF component manager has restored/fetched managed components.
2. CLI mode — called with a file path by the CMake hook.

Registered via platformio_options.extra_scripts in __init__.py.
"""

import os
import re
import sys

MARKER = "// patched: icy header extraction"
CMAKE_MARKER = "# [icy-patch]"
PATCH_VERSION = "volatile int g_icy_patch_version = 1;"


# ─────────────────────────────────────────────────────────────────────
# Core patch logic (mode-independent)
# ─────────────────────────────────────────────────────────────────────

def _apply_patch(http_c):
    """Apply and verify the ICY header extraction patch."""
    with open(http_c, "r") as f:
        src = f.read()

    modified = False

    # ── Migrate old definition → extern declaration ──
    old_def = "volatile int g_icy_metaint = 0;"
    if old_def in src:
        src = src.replace(old_def, "extern volatile int g_icy_metaint;", 1)
        modified = True
        print("  [patch] migrated g_icy_metaint definition to extern")

    # ── Check what's already patched ──
    has_metaint_decl = "extern volatile int g_icy_metaint;" in src
    has_bitrate_decl = "extern volatile int g_icy_bitrate;" in src
    has_metaint_check = 'strcasecmp(evt->header_key, "icy-metaint")' in src
    has_bitrate_check = 'strcasecmp(evt->header_key, "icy-br")' in src
    has_patch_version = PATCH_VERSION in src

    tag_line = 'static const char *TAG = "ESP_GMF_HTTP";'

    # ── Add extern declarations after TAG ──
    if not has_metaint_decl:
        if tag_line not in src:
            print("  [patch] TAG line not found — skipping")
            return False
        src = src.replace(
            tag_line,
            f"{tag_line}\n\n"
            f"extern volatile int g_icy_metaint;  {MARKER}\n"
            f"extern volatile int g_icy_bitrate;",
        )
        modified = True
    elif not has_bitrate_decl:
        src = src.replace(
            "extern volatile int g_icy_metaint;",
            "extern volatile int g_icy_metaint;\n"
            "extern volatile int g_icy_bitrate;",
            1,
        )
        modified = True

    if not has_patch_version:
        bitrate_decl = "extern volatile int g_icy_bitrate;"
        if bitrate_decl not in src:
            print("  [patch] bitrate declaration not found", file=sys.stderr)
            return False
        src = src.replace(
            bitrate_decl,
            f"{bitrate_decl}\n{PATCH_VERSION}",
            1,
        )
        modified = True

    # ── Add header checks in _http_event_handle ──
    if not has_metaint_check:
        # Fresh file — insert both checks before the final return
        old_return = (
            "    }\n"
            "    return ESP_GMF_ERR_OK;\n"
            "}\n"
            "\n"
            "static int dispatch_hook"
        )
        new_return = (
            "    }\n"
            '    if (strcasecmp(evt->header_key, "icy-metaint") == 0) {\n'
            "        g_icy_metaint = atoi(evt->header_value);\n"
            "    }\n"
            '    if (strcasecmp(evt->header_key, "icy-br") == 0) {\n'
            "        g_icy_bitrate = atoi(evt->header_value);\n"
            "    }\n"
            "    return ESP_GMF_ERR_OK;\n"
            "}\n"
            "\n"
            "static int dispatch_hook"
        )
        if old_return in src:
            src = src.replace(old_return, new_return, 1)
            modified = True
        else:
            print("  [patch] insertion point not found — skipping")
            return False
    elif not has_bitrate_check:
        # Has metaint check but not bitrate — add after metaint block
        metaint_block = (
            '    if (strcasecmp(evt->header_key, "icy-metaint") == 0) {\n'
            "        g_icy_metaint = atoi(evt->header_value);\n"
            "    }\n"
        )
        upgraded = (
            metaint_block
            + '    if (strcasecmp(evt->header_key, "icy-br") == 0) {\n'
            "        g_icy_bitrate = atoi(evt->header_value);\n"
            "    }\n"
        )
        src = src.replace(metaint_block, upgraded, 1)
        modified = True

    # ── Add stdlib.h for atoi() ──
    if "#include <stdlib.h>" not in src:
        src = "#include <stdlib.h>\n" + src
        modified = True

    # ── Write ──
    if modified:
        with open(http_c, "w") as f:
            f.write(src)
        what = []
        if not has_metaint_check:
            what.append("icy-metaint")
        if not has_bitrate_check:
            what.append("icy-br")
        print(f"  [patch] esp_gmf_io_http.c: {'added' if what else 'updated'}"
              f"{' ' + '+'.join(what) if what else ''} extraction")
    else:
        print("  [patch] esp_gmf_io_http.c: already patched (icy headers)")

    required = (
        "extern volatile int g_icy_metaint;",
        "extern volatile int g_icy_bitrate;",
        PATCH_VERSION,
        'strcasecmp(evt->header_key, "icy-metaint")',
        'strcasecmp(evt->header_key, "icy-br")',
    )
    if not all(item in src for item in required):
        print("  [patch] ICY patch verification failed", file=sys.stderr)
        return False
    return True


# ─────────────────────────────────────────────────────────────────────
# CLI mode: called from cmake with explicit file path
# ─────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if len(sys.argv) > 1:
        path = sys.argv[1]
        if os.path.isfile(path):
            if not _apply_patch(path):
                sys.exit(1)
        else:
            print(f"  [patch] File not found: {path}", file=sys.stderr)
            sys.exit(1)
    sys.exit(0)


# ─────────────────────────────────────────────────────────────────────
# PlatformIO pre: script mode
# ─────────────────────────────────────────────────────────────────────

Import("env")  # noqa: F821 — PlatformIO/SCons built-in


def _inject_cmake_hook(cmake_lists_path, patch_script_path):
    """Inject CMake code that patches after managed-component restoration.

    ESP-IDF's component manager may restore managed sources during project(),
    overwriting any earlier edit. Running after project() guarantees the
    compiler sees the patched source on both clean and incremental builds.
    """
    if not os.path.isfile(cmake_lists_path):
        raise RuntimeError("CMakeLists.txt not found; cannot install ICY patch")

    with open(cmake_lists_path, "r") as f:
        content = f.read()

    if CMAKE_MARKER in content:
        return  # Already injected (shouldn't happen — ESPHome regenerates)

    # Escape backslashes for cmake paths (Windows)
    escaped_script = patch_script_path.replace("\\", "/")

    snippet = (
        f"\n{CMAKE_MARKER} Patch GMF HTTP IO for ICY metadata extraction\n"
        'set(_gmf_http "${CMAKE_SOURCE_DIR}/managed_components/'
        'espressif__gmf_io/esp_gmf_io_http.c")\n'
        'if(EXISTS "${_gmf_http}")\n'
        "    execute_process(\n"
        f'        COMMAND "${{PYTHON}}" "{escaped_script}" "${{_gmf_http}}"\n'
        "        RESULT_VARIABLE _icy_rc\n"
        "    )\n"
        "    if(_icy_rc EQUAL 0)\n"
        '        message(STATUS "[icy-patch] Patched esp_gmf_io_http.c")\n'
        "    else()\n"
        "        message(FATAL_ERROR "
        '"[icy-patch] Failed to patch esp_gmf_io_http.c")\n'
        "    endif()\n"
        "else()\n"
        '    message(FATAL_ERROR "[icy-patch] esp_gmf_io_http.c not found")\n'
        "endif()\n"
    )

    # Insert after the project(...) line
    updated = re.sub(
        r"(project\([^)]+\))",
        r"\1" + snippet,
        content,
        count=1,
    )
    if updated == content:
        raise RuntimeError("project() not found; cannot install ICY patch")

    with open(cmake_lists_path, "w") as f:
        f.write(updated)
    print("  [patch] Injected cmake hook for deferred ICY patching")


def _current_script_path(env):
    """Return this PlatformIO extra script path.

    Recent PlatformIO/SCons versions execute extra scripts without defining
    __file__, so derive the path from PlatformIO's registered pre scripts.
    """
    if "__file__" in globals():
        return os.path.abspath(globals()["__file__"])

    for script in env.GetExtraScripts("pre"):
        if script.startswith("pre:"):
            script = script[4:]
        script = env.subst(script)
        if os.path.basename(script) == "patch_gmf_http.py":
            return os.path.abspath(script)

    raise RuntimeError("Unable to resolve patch_gmf_http.py path")


def _patch(env):
    project_dir = env.subst("$PROJECT_DIR")
    cmake_lists = os.path.join(project_dir, "CMakeLists.txt")
    patch_script = _current_script_path(env)
    print("  [patch] injecting post-project CMake hook")
    _inject_cmake_hook(cmake_lists, patch_script)


_patch(env)  # noqa: F821
