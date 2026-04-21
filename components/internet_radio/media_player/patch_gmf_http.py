"""Pre-build script: patch ESP-GMF HTTP IO for ICY header extraction.

esp_gmf_io_http.c's internal _http_event_handle only processes
Content-Encoding headers.  This patch adds extraction of icy-metaint
and icy-br response headers into global volatiles that
internet_radio.cpp reads on the first ON_RESPONSE callback.

Idempotent: checks for each patch component individually.
After patching, deletes cached object files to force recompilation.

Registered via platformio_options.extra_scripts in __init__.py.
"""
Import("env")  # noqa: F821 — PlatformIO/SCons built-in

import os
import glob as globmod

MARKER = "// patched: icy header extraction"


def _invalidate_obj(env, src_path):
    """Delete cached object files for a source to force recompilation."""
    base = os.path.splitext(os.path.basename(src_path))[0]
    build_dir = env.subst("$BUILD_DIR")
    pattern = os.path.join(build_dir, "**", base + ".*o*")
    for obj in globmod.glob(pattern, recursive=True):
        if obj.endswith(".o") or obj.endswith(".obj"):
            os.remove(obj)
            print(f"  [patch] deleted cached {os.path.relpath(obj, build_dir)}")


def _patch(env):
    managed = os.path.join(
        env.subst("$PROJECT_DIR"), "managed_components", "espressif__gmf_io"
    )
    http_c = os.path.join(managed, "esp_gmf_io_http.c")

    if not os.path.isfile(http_c):
        print("  [patch] esp_gmf_io_http.c not found — skipping ICY patch")
        return

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
            return
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
            return
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

    # ── Write and invalidate cache ──
    if modified:
        with open(http_c, "w") as f:
            f.write(src)
        _invalidate_obj(env, http_c)
        what = []
        if not has_metaint_check:
            what.append("icy-metaint")
        if not has_bitrate_check:
            what.append("icy-br")
        print(f"  [patch] esp_gmf_io_http.c: {'added' if what else 'updated'}"
              f"{' ' + '+'.join(what) if what else ''} extraction")
    else:
        print("  [patch] esp_gmf_io_http.c: already patched (icy headers)")


_patch(env)  # noqa: F821
