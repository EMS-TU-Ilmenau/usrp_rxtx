#!/usr/bin/python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MIT
# (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

import asyncio
import asyncio_mqtt
import calendar
import json
import io
import os
import pathlib
import termios
from typing import Self
from ubx_parser import *



async def asyncio_open_tty(
    tty_path: str,
    iospeed: int = termios.B115200
) -> (asyncio.StreamReader, asyncio.StreamWriter):
    if os.name != "posix":
        raise NotImplementedError(f"unsupported operating system: {os.name}")

    # open raw file descriptor for tty
    tty_fd = os.open(tty_path, os.O_RDWR | os.O_CLOEXEC | os.O_NOCTTY | os.O_NONBLOCK)

    # configure tty for raw communication
    attr = termios.tcgetattr(tty_fd)
    # [iflag, oflag, cflag, lflag, ispeed, ospeed, cc]
    attr[0] = 0
    attr[1] = 0
    attr[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
    attr[3] = 0
    attr[4] = iospeed
    attr[5] = iospeed
    # disable all control characters
    attr[6] = [b"\x00"] * len(attr[6])
    # configure polling read (non-blocking)
    attr[6][termios.VMIN] = 0
    attr[6][termios.VTIME] = 0
    termios.tcsetattr(tty_fd, termios.TCSAFLUSH, attr)

    # construct regular Python file (RawIOBase) from open file descriptor
    tty = open(tty_fd, "r+b", buffering=0)

    # create asyncio.StreamReader
    loop = asyncio.get_event_loop()
    reader = asyncio.StreamReader()
    protocol = asyncio.StreamReaderProtocol(reader)
    await loop.connect_read_pipe(lambda: protocol, tty)

    # create asyncio.StreamWriter
    transport, protocol = await loop.connect_write_pipe(asyncio.streams.FlowControlMixin, tty)
    writer = asyncio.StreamWriter(transport, protocol, reader, loop)

    return reader, writer


class UbloxF9:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self._reader = reader
        self._writer = writer
        self._parser = AsyncUbxParser(self._reader)

    def __aiter__(self) -> Self:
        return self

    async def __anext__(self) -> UbxFrame | UbxMessage:
        return await self._parser.__anext__()

    async def cfg_get(self, layer: UbxCfgValget.LAYER, keys: list[int]) -> dict[int, int]:
        if len(keys) > 64:
            raise NotImplementedError

        data: dict[int, int] = {}
        pos = 0
        while True:
            valget = UbxCfgValgetPoll(layer, pos, keys)
            self._writer.write(valget.serialize())
            await self._writer.drain()

            async for msg in self._parser:
                if isinstance(msg, UbxAckAck) and (msg._cls, msg._id) == valget.TYPE.value:
                    pass
                elif isinstance(msg, UbxAckNak) and (msg._cls, msg._id) == valget.TYPE.value:
                    # contrary to the documentation, ZED-F9P will return ACK-NAK
                    # when query result is empty
                    return data
                elif isinstance(msg, UbxCfgValgetPolled):
                    data.update(msg._data)
                    if len(msg._data) < 64:
                        return data
                    else:
                        break
            pos += 64

    async def cfg_set(self, layer: UbxCfgValset.LAYER, data: dict[int, int]) -> None:
        if len(data) > 64:
            raise NotImplementedError

        valset = UbxCfgValset(layer, UbxCfgValset.TRANSACTION.NONE, data)
        self._writer.write(valset.serialize())
        await self._writer.drain()

        async for msg in self._parser:
            if isinstance(msg, UbxAckAck) and (msg._cls, msg._id) == valset.TYPE.value:
                return
            elif isinstance(msg, UbxAckNak) and (msg._cls, msg._id) == valset.TYPE.value:
                raise RuntimeError("UBX-CFG-VALSET failed: received CFG-ACK-NAK")


class StreamReaderTee:
    def __init__(self, reader: asyncio.StreamReader, file: io.FileIO):
        self._file = file
        self._reader = reader

    async def __aenter__(self) -> Self:
        self._file.__enter__()
        return self

    async def __aexit__(self, exc_type, exc_value, traceback):
        self._file.__exit__(exc_type, exc_value, traceback)

    async def read(self, n: int = -1) -> bytes:
        buf = await self._reader.read(n)
        written = self._file.write(buf)
        # https://docs.python.org/3/library/io.html#io.BufferedIOBase.write
        assert written == len(buf)
        return buf


class AsyncMqttRtk(asyncio_mqtt.AsyncMqttClientLog):
    def on_connect(self, client, userdata, flags, reason_code, properties):
        super().on_connect(client, userdata, flags, reason_code, properties)
        self.client.subscribe("usrp_rxtx/rtcm")

    def on_reconnect_error(self, reason_code, exception):
        # suppress logging of reconnect errors
        pass

async def rtk_parse(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
    mqtt: AsyncMqttRtk,
    topic: str
):
    # TODO: load this from config file
    cfg_chunked = [
        # configure solution and UBX messages
        {0x20110021: 0, 0x1031001f: 1, 0x10310001: 1, 0x10310003: 1, 0x10310020: 0, 0x10310005: 0, 0x10310021: 1, 0x10310007: 1, 0x1031000a: 1, 0x10310022: 0, 0x1031000d: 0, 0x1031000e: 0, 0x10310024: 0, 0x10310012: 0, 0x10310015: 0, 0x10310025: 1, 0x10310018: 1, 0x1031001a: 1, 0x30210001: 100, 0x30210002: 1, 0x20910352: 1, 0x2091035c: 1, 0x20910162: 1, 0x20910009: 1, 0x20910036: 1, 0x20910090: 1, 0x20910018: 10, 0x20910348: 10, 0x2091001d: 10, 0x20910045: 1, 0x209102a7: 1, 0x20910234: 1, 0x2091026b: 1},
        # disable useless NMEA sentences
        {0x20920006: 0x00, 0x20920007: 0x00, 0x20920008: 0x00, 0x20920009: 0x07, 0x2092000a: 0x00, 0x209100ba: 0x00, 0x209100be: 0x00, 0x209100bb: 0x00, 0x209100bc: 0x00, 0x209100bd: 0x00, 0x209100c9: 0x00, 0x209100cd: 0x00, 0x209100ca: 0x00, 0x209100cb: 0x00, 0x209100cc: 0x00, 0x209100bf: 0x00, 0x209100c3: 0x00, 0x209100c0: 0x00, 0x209100c1: 0x00, 0x209100c2: 0x00, 0x209100c4: 0x00, 0x209100c8: 0x00, 0x209100c5: 0x00, 0x209100c6: 0x00, 0x209100c7: 0x00, 0x209100ab: 0x00, 0x209100af: 0x00, 0x209100ac: 0x00, 0x209100ad: 0x00, 0x209100ae: 0x00, 0x209100b0: 0x00, 0x209100b4: 0x00, 0x209100b1: 0x00, 0x209100b2: 0x00, 0x209100b3: 0x00}
    ]

    rtk = UbloxF9(reader, writer)

    # verify BBR and FLASH configuration layers are empty
    if len(await rtk.cfg_get(UbxCfgValget.LAYER.BBR, [0x0FFF0000])) > 0:
        raise RuntimeError("BBR configuration layer not empty; configuration reset required")
    if len(await rtk.cfg_get(UbxCfgValget.LAYER.FLASH, [0x0FFF0000])) > 0:
        raise RuntimeError("FLASH configuration layer not empty; configuration reset required")

    # download DEFAULT and RAM configuration from device
    cfg_default = await rtk.cfg_get(UbxCfgValget.LAYER.DEFAULT, [0x0FFF0000])
    cfg_ram = await rtk.cfg_get(UbxCfgValget.LAYER.RAM, [0x0FFF0000])

    # compute target configuration
    cfg_target = cfg_default.copy()
    for cfg_data in cfg_chunked:
        cfg_target.update(cfg_data)

    # only configure device if RAM configuration differs from target
    if cfg_ram != cfg_target:
        for cfg_data in cfg_chunked:
            await rtk.cfg_set({UbxCfgValset.LAYER.RAM}, cfg_data)

        # compare configuration result
        cfg_ram = await rtk.cfg_get(UbxCfgValget.LAYER.RAM, [0x0FFF0000])
        if cfg_ram != cfg_target:
            raise RuntimeError("failed configuring device")

    time_lastmsg = None
    async for msg in rtk:
        if isinstance(msg, UbxNavPvt):
            try:
                time_ns = calendar.timegm((msg._year, msg._month, msg._day,
                                             msg._hour, msg._min, msg._sec)) \
                          * 1_000_000_000 + msg._nano
            except ValueError:
                time_ns = -1

            # publish position twice a second, aligned to .0 and .5 seconds.
            # accept up to 20 ms premature PVT to avoid delaying NAV-PVT with
            # negative nanosecond time component (msg._nano)
            time_halfsec = int(time_ns * 2e-9 + 0.02 * 2.)
            if time_halfsec == time_lastmsg:
                continue
            else:
                time_lastmsg = time_halfsec

            data = {
                "time_ns": time_ns,
                "fixType": msg.fixType.name,
                "gnssFixOK": msg.gnssFixOK,
                "numSV": msg._numSV,
                "carrSoln": msg.carrSoln.name,
                "lastCorrectionAge": msg.lastCorrectionAge.name,
                "acc3D": round(msg.acc_3d, 3),
                "lat": round(msg.lat, 7),
                "lon": round(msg.lon, 7),
                "hMSL": round(msg.hMSL, 3)
            }

            # publish data, ignoring any errors
            mqtt.publish(topic, json.dumps(data))

async def rtcm_feed(mqtt: AsyncMqttRtk, writer: asyncio.StreamWriter):
    async for msg in mqtt:
        writer.write(msg.payload)
        await writer.drain()

async def main(tty_path: pathlib.Path):
    import signal
    import socket
    import time

    hostname = socket.gethostname()
    loop = asyncio.get_running_loop()

    # open TTY and wrap reader into logging Tee instance
    reader, writer = await asyncio_open_tty(str(tty_path))
    logfile = open(f"rtk_{hostname}_{tty_path.name}_{int(time.time()):d}.ubx", "xb")
    reader = StreamReaderTee(reader, logfile)

    # configure MQTT client
    mqtt_client = AsyncMqttRtk("192.0.2.1", async_connect=True,
                               connect_delay=[0., 1., 2., 3., 5.],
                               keepalive=5)

    async with reader, mqtt_client:
        # run all worker coroutines asynchronously
        task = asyncio.gather(
            rtcm_feed(mqtt_client, writer),
            rtk_parse(reader, writer, mqtt_client, f"usrp_rxtx/rtk/{hostname}/{tty_path.name}"),
        )

        # setup graceful termination by cancelling tasks on SIGINT/SIGTERM
        if os.name == "posix":
            loop.add_signal_handler(signal.SIGINT,  lambda: task.cancel())
            loop.add_signal_handler(signal.SIGTERM, lambda: task.cancel())

        # wait for all tasks to finish (on exception or after termination)
        await task

if __name__ == "__main__":
    import logging
    import sys

    logging.basicConfig(level=logging.INFO)

    try:
        asyncio.run(main(pathlib.Path(sys.argv[1])))
    except (asyncio.CancelledError, KeyboardInterrupt):
        pass
