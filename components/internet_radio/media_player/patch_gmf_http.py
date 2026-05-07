"""Pre-build script: patch ESP-GMF HTTP IO for ICY header extraction.

esp_gmf_io_http.c's internal _http_event_handle only processes
Content-Encoding headers.  This patch adds extraction of icy-metaint
and icy-br response headers into global volatiles that
internet_radio.cpp reads on the first ON_RESPONSE callback.

Idempotent: checks for each patch component individually.
After patching, deletes cached object files to force recompilation.

Operates in two modes:
1. PlatformIO pre: script — patches directly if managed_components exist,
   otherwise injects a cmake hook for first-time builds (e.g. HA addon).
2. CLI mode — called with file path argument by the cmake hook.

Registered via platformio_options.extra_scripts in __init__.py.
"""

import os
import re
import sys
import glob as globmod

MARKER = "// patched: icy header extraction"
CMAKE_MARKER = "# [icy-patch]"


# ─────────────────────────────────────────────────────────────────────
# Core patch logic (mode-independent)
# ─────────────────────────────────────────────────────────────────────

def _apply_patch(http_c):
    """Apply ICY header extraction patch. Returns True if file was modified."""
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
        return True

    print("  [patch] esp_gmf_io_http.c: already patched (icy headers)")
    return False


# ─────────────────────────────────────────────────────────────────────
# CLI mode: called from cmake with explicit file path
# ─────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if len(sys.argv) > 1:
        path = sys.argv[1]
        if os.path.isfile(path):
            _apply_patch(path)
        else:
            print(f"  [patch] File not found: {path}", file=sys.stderr)
            sys.exit(1)
    sys.exit(0)


# ─────────────────────────────────────────────────────────────────────
# PlatformIO pre: script mode
# ─────────────────────────────────────────────────────────────────────

Import("env")  # noqa: F821 — PlatformIO/SCons built-in


def _invalidate_obj(env, src_path):
    """Delete cached object files for a source to force recompilation."""
    base = os.path.splitext(os.path.basename(src_path))[0]
    build_dir = env.subst("$BUILD_DIR")
    pattern = os.path.join(build_dir, "**", base + ".*o*")
    for obj in globmod.glob(pattern, recursive=True):
        if obj.endswith(".o") or obj.endswith(".obj"):
            os.remove(obj)
            print(f"  [patch] deleted cached {os.path.relpath(obj, build_dir)}")


def _inject_cmake_hook(cmake_lists_path, patch_script_path):
    """Inject cmake snippet into CMakeLists.txt to patch after component fetch.

    On fresh builds (e.g. HA addon), managed_components/ doesn't exist when
    this PlatformIO pre: script runs.  The IDF component manager fetches them
    during cmake's project() call.  By injecting a snippet *after* project(),
    the patch runs at the right time — components exist, but haven't been
    compiled yet.
    """
    if not os.path.isfile(cmake_lists_path):
        print("  [patch] CMakeLists.txt not found — cannot inject cmake hook")
        return

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
        "        message(WARNING "
        '"[icy-patch] Failed to patch esp_gmf_io_http.c")\n'
        "    endif()\n"
        "else()\n"
        '    message(STATUS "[icy-patch] esp_gmf_io_http.c not found")\n'
        "endif()\n"
    )

    # Insert after the project(...) line
    content = re.sub(
        r"(project\([^)]+\))",
        r"\1" + snippet,
        content,
        count=1,
    )

    with open(cmake_lists_path, "w") as f:
        f.write(content)
    print("  [patch] Injected cmake hook for deferred ICY patching")


def _patch(env):
    project_dir = env.subst("$PROJECT_DIR")
    http_c = os.path.join(
        project_dir, "managed_components", "espressif__gmf_io",
        "esp_gmf_io_http.c",
    )

    if os.path.isfile(http_c):
        # Rebuild case — managed_components already exist, patch directly
        if _apply_patch(http_c):
            _invalidate_obj(env, http_c)
    else:
        # Fresh build — inject cmake hook to patch after component fetch
        print("  [patch] esp_gmf_io_http.c not found"
              " — injecting cmake hook for deferred patching")
        cmake_lists = os.path.join(project_dir, "CMakeLists.txt")
        patch_script = os.path.abspath(__file__)
        _inject_cmake_hook(cmake_lists, patch_script)


_patch(env)  # noqa: F821
