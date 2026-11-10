"""Shared GMF transport, codec memory and lifecycle support."""

import esphome.codegen as cg
from esphome.components import esp32

gmf_audio_ns = cg.esphome_ns.namespace("gmf_audio")
DEPENDENCIES = ["esp32", "network"]


async def to_code(config):
    esp32.include_builtin_idf_component("esp_http_client")
    esp32.add_idf_component(name="espressif/esp_audio_simple_player", ref="0.9.6~1")
    for option in (
        "CONFIG_ESP_AUDIO_SIMPLE_PLAYER_HTTP_EN",
        "CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN",
        "CONFIG_ESP_AUDIO_SIMPLE_PLAYER_FILE_EN",
        "CONFIG_ESP_AUDIO_SIMPLE_PLAYER_EMBED_FLASH_EN",
    ):
        esp32.add_idf_sdkconfig_option(option, False)
    for option in (
        "CONFIG_LIBC_NEWLIB",
        "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC",
        "CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS",
        "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE",
        "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL",
    ):
        esp32.add_idf_sdkconfig_option(option, True)
    esp32.add_idf_sdkconfig_option("CONFIG_ESP_HTTP_CLIENT_MAX_SAVED_RESPONSE_HEADERS", 20)
