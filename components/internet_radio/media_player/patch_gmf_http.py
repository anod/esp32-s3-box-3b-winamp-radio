"""Pre-build script: patch ESP-GMF HTTP IO for ICY metadata support.

esp_gmf_io_http.c's internal _http_event_handle only processes
Content-Encoding headers.  This patch adds icy-metaint extraction
into a global volatile int that internet_radio.cpp reads on the
first ON_RESPONSE callback to know the ICY metadata interval.

Registered via platformio_options.extra_scripts in __init__.py.
"""
Import("env")  # noqa: F821 — PlatformIO/SCons built-in

import os

MARKER = "// patched: icy-metaint extraction"


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

    if MARKER in src:
        print("  [patch] esp_gmf_io_http.c: already patched (icy-metaint)")
        return

    # 1. Add global variable after TAG
    tag_line = 'static const char *TAG = "ESP_GMF_HTTP";'
    if tag_line not in src:
        print("  [patch] esp_gmf_io_http.c: TAG line not found — skipping")
        return
    src = src.replace(
        tag_line,
        f"{tag_line}\n\n"
        f"volatile int g_icy_metaint = 0;  {MARKER}",
    )

    # 2. Add icy-metaint check in _http_event_handle, before the final return
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
        "    return ESP_GMF_ERR_OK;\n"
        "}\n"
        "\n"
        "static int dispatch_hook"
    )
    if old_return in src:
        src = src.replace(old_return, new_return, 1)
    else:
        print("  [patch] esp_gmf_io_http.c: insertion point not found — skipping")
        return

    # 3. Add stdlib.h for atoi() if not present
    if "#include <stdlib.h>" not in src:
        src = "#include <stdlib.h>\n" + src

    with open(http_c, "w") as f:
        f.write(src)

    print("  [patch] esp_gmf_io_http.c: added icy-metaint extraction")


_patch(env)  # noqa: F821
