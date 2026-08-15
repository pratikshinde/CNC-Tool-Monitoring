#!/usr/bin/env python
"""Gzip a single file for the `web` LittleFS partition image.

Invoked from main/CMakeLists.txt at build time, not on-device: the ESP32
has no spare cycles to compress on the fly for a request that already
shares CORE_NETWORK with WiFi, Modbus and OTA (see web.c's handle_index()).

Usage: gzip_asset.py <src> <dst>
"""
import gzip
import shutil
import sys

with open(sys.argv[1], 'rb') as src, gzip.open(sys.argv[2], 'wb', compresslevel=9) as dst:
    shutil.copyfileobj(src, dst)
