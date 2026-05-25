#!/usr/bin/python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MIT
# (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

import asyncio
import base64
import logging
import time
from typing import Self
import urllib.parse

_logger = logging.getLogger(__name__)


class NtripError(Exception):
    pass


class NtripClient:
    _POS_UPDATE_INTERVAL = 10.

    def __init__(self, ntrip_url: str, lat: float, lon: float, alt: float, timeout: float = 10.):
        # validate url
        url = urllib.parse.urlparse(ntrip_url)
        if url.scheme != "http":
            raise ValueError(f"invalid URL scheme: {url.scheme}")
        self._host = url.hostname
        self._port = url.port

        # copy parameters
        self._lat = float(lat)
        self._lon = float(lon)
        self._alt = float(alt)
        self._timeout = float(timeout)

        # prepare HTTP GET request
        http_auth = base64.b64encode(f"{url.username}:{url.password}".encode("ascii")).decode("ascii")
        self._http_req = (
            f"GET {url.path} HTTP/1.1\r\n"
            f"Host: {url.path}\r\n"
            f"Authorization: Basic {http_auth}\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        ).encode("ascii")

        self._connected = False
        self._next_pos_update = 0.

    def __aiter__(self) -> Self:
        return self

    async def __anext__(self) -> bytes:
        while True:
            # connect to Ntrip caster if not already connected
            if not self._connected:
                try:
                    await asyncio.wait_for(self._connect(), self._timeout)
                except asyncio.TimeoutError:
                    _logger.error("timeout occurred connecting to Ntrip caster")
                    continue
                # handle typical network errors, e.g., TimeoutError,
                # ConnectionRefusedError, i.e., OSError(errno.ECONNREFUSED),
                # OSError(errno.ENETDOWN), OSError(errno.ENETUNREACH),
                # OSError(errno.EHOSTUNREACH), socket.gaierror, etc.
                except OSError as e:
                    _logger.error(f"exception occurred connecting to Ntrip caster: {e}")
                    await asyncio.sleep(1)
                    continue

            # periodically send NMEA position string (initially required for
            # caster to start sending correction data and to keep it from
            # closing the connection after its timeout interval)
            if time.monotonic() > self._next_pos_update:
                try:
                    self._writer.write(self._get_nmea_pos_update().encode("ascii"))
                    await asyncio.wait_for(self._writer.drain(), self._timeout)
                    self._next_pos_update = time.monotonic() + self._POS_UPDATE_INTERVAL
                except asyncio.TimeoutError:
                    _logger.error("timeout occurred sending position update to Ntrip caster")
                    await self._disconnect()
                    continue

            # read RTCM messages from caster
            try:
                rtcm = await asyncio.wait_for(self._reader.read(4096), self._timeout)
                if not rtcm:
                    _logger.error("Ntrip caster closed connection unexpectedly")
                    await self._disconnect()
                return rtcm
            except asyncio.TimeoutError:
                _logger.error("timeout occurred receiving RTCM data from Ntrip caster")
                await self._disconnect()

    async def _connect(self) -> None:
        # send HTTP GET request
        self._reader, self._writer = await asyncio.open_connection(self._host, self._port)
        self._writer.write(self._http_req)
        await self._writer.drain()

        # wait for and verify server HTTP response header (terminated by \r\n\r\n)
        resp = await self._reader.readuntil(b"\r\n\r\n")
        if resp != b"ICY 200 OK\r\n\r\n":
            raise NtripError(f"Ntrip caster sent invalid response: {resp}")

        self._connected = True
        self._next_pos_update = 0.

    async def _disconnect(self) -> None:
        self._writer.close()
        await self._writer.wait_closed()

        self._connected = False
        del self._reader
        del self._writer

    def _get_nmea_pos_update(self) -> str:
        # construct GNGGA sentence
        now = time.gmtime()
        lat_int, lat_frac = divmod(abs(self._lat), 1.)
        lon_int, lon_frac = divmod(abs(self._lon), 1.)
        gga = f"GNGGA,{now.tm_hour:02d}{now.tm_min:02d}{now.tm_sec:02d}.00," \
              f"{lat_int:02.0f}{lat_frac*60.:08.5f},{'N' if self._lat >= 0. else 'S'}," \
              f"{lon_int:03.0f}{lon_frac*60.:08.5f},{'E' if self._lon >= 0. else 'W'}," \
              f"1,12,0.0,{self._alt:.1f},M,,,,"

        # compute NMEA frame checksum
        csum = 0
        for c in gga:
            csum ^= ord(c)

        # compose full NMEA sentence
        return f"${gga}*{csum:02x}\r\n"


async def main():
    ntrip = NtripClient("http://user:passwort@ntrip-caster-address:port/path", 50.68221389, 10.93868317, 512.331)
    async for rtcm in ntrip:
        print(len(rtcm))


if __name__ == "__main__":
    asyncio.run(main())
