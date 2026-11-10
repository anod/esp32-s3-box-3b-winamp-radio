"""Shared radio-only PCM spectrum capture, independent of the display backend."""

import esphome.codegen as cg
import esphome.config_validation as cv

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    cg.add(cg.RawExpression("spectrum_init()"))
