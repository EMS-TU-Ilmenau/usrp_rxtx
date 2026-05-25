#!/usr/bin/python3
# -*- coding: utf-8 -*-
# (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

import asyncio
import asyncio_mqtt
import calendar
import enum
import json
import io
import os
import pathlib
import socket
import termios
import time
from typing import Self


async def asyncio_open_tty(
    tty_path: str,
    iospeed: int = termios.B115200
) -> asyncio.StreamReader:
    if os.name != "posix":
        raise NotImplementedError(f"unsupported operating system: {os.name}")

    # open raw file descriptor for tty
    tty_fd = os.open(tty_path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOCTTY | os.O_NONBLOCK)

    # configure tty for raw communication
    attr = termios.tcgetattr(tty_fd)
    # [iflag, oflag, cflag, lflag, ispeed, ospeed, cc]
    attr[0] = termios.IGNBRK | termios.IGNPAR
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
    tty = open(tty_fd, "rb", buffering=0)

    # create asyncio.StreamReader
    loop = asyncio.get_event_loop()
    reader = asyncio.StreamReader()
    protocol = asyncio.StreamReaderProtocol(reader)
    await loop.connect_read_pipe(lambda: protocol, tty)

    return reader


class HealthState(enum.Enum):
    # https://www.jackson-labs.com/assets/uploads/main/LC_XO_Manual.pdf#page=41
    COARSE_DAC_MAX = 0x01
    COARSE_DAC_MIN = 0x02
    PHASE_OFFSET_HIGH = 0x04
    RUN_TIME_LOW = 0x08
    HOLDOVER = 0x10
    FREQ_ESTIMATE_UNBOUNDED = 0x20
    ADEV_HIGH = 0x100
    SETTLING = 0x200


class LockState(enum.Enum):
    # https://www.jackson-labs.com/assets/uploads/main/LC_XO_Manual.pdf#page=46
    WARMUP = 0
    HOLDOVER = 1
    LOCKING = 2
    HOLDOVER_LOCKED = 5
    LOCKED = 6
    INVALID = None


class AsyncLogLCXO:
    def __init__(self, reader: asyncio.StreamReader, name: str):
        self._reader = reader
        self._name = name
        self._hostname = socket.gethostname()

    async def __aenter__(self) -> Self:
        self._log_file = open(f"lc_xo_{self._hostname}_{self._name}_{int(time.time())}.log", "xb")
        self._log_file.__enter__()

    async def __aexit__(self, exc_type, exc_value, traceback):
        self._log_file.__exit__(exc_type, exc_value, traceback)

    def __aiter__(self) -> Self:
        return self

    async def __anext__(self) -> str:
        while True:
            # read single line
            buf = await self._reader.readuntil(b"\r\n")
            if len(buf) == 0:
                raise AsyncStopIteration

            # strip leading NUL bytes and write to log file
            buf = buf.lstrip(b"\0")
            written = self._log_file.write(buf)
            # https://docs.python.org/3/library/io.html#io.BufferedIOBase.write
            assert written == len(buf)

            try:
                return buf.removesuffix(b"\r\n").decode("ascii")
            except:
                continue


class AsyncMqttClient(asyncio_mqtt.AsyncMqttClientLog):
    def on_reconnect_error(self, reason_code, exception):
        # suppress logging of reconnect errors
        pass


async def main(tty_path: pathlib.Path):
    import re
    import signal
    import time

    hostname = socket.gethostname()
    loop = asyncio.get_running_loop()

    # setup graceful termination by cancelling tasks on SIGINT/SIGTERM
    if os.name == "posix":
        task = asyncio.current_task()
        loop.add_signal_handler(signal.SIGINT,  lambda: task.cancel())
        loop.add_signal_handler(signal.SIGTERM, lambda: task.cancel())

    # https://www.jackson-labs.com/assets/uploads/main/LC_XO_Manual.pdf#page=45
    # 08-07-31 373815 60685 -32.08 -2.22E-11 14 10 6 0x54
    re_trace = re.compile(r"^(\d{2}-\d{2}-\d{2}) (\d+) (\d+) ([-\d\.]+) ([-\d\.E]+) (\d+) (\d+) (\d+) (0x[0-9a-fA-F]+)$")

    # open LC_XO
    lcxo = AsyncLogLCXO(await asyncio_open_tty(str(tty_path)), tty_path.name)

    # configure MQTT client
    mqtt_client = AsyncMqttClient("192.0.2.1", async_connect=True,
                                  connect_delay=[0., 1., 2., 3., 5.],
                                  keepalive=5)
    mqtt_topic = f"usrp_rxtx/gnssdo/lc_xo/{hostname}/{tty_path.name}"

    async with lcxo, mqtt_client:
        data = {}
        async for line in lcxo:
            # parse clock servo trace
            if m := re_trace.match(line):
                try:
                    date = m.group(1)
                    pps_count = int(m.group(2))
                    dac = int(m.group(3))
                    offset_ns = float(m.group(4))
                    freq_err = float(m.group(5))
                    sats_vis = int(m.group(6))
                    sats_tracked = int(m.group(7))
                    lock_state = LockState(int(m.group(8)))
                    health_hex = int(m.group(9), 16)
                except:
                    continue

                health_state = set()
                for bit in HealthState:
                    if health_hex & bit.value:
                        health_state.add(bit)

                data["time_ns"] = None
                data["lock"] = lock_state.name
                data["num_sats"] = sats_tracked
                data["offset_ns"] = offset_ns
                data["stationary"] = False
                if len(health_state) > 0:
                    data["warning"] = sorted([bit.name for bit in health_state])

            # parse time from GPRMC sentence and send MQTT message
            elif line.startswith("$GPRMC,"):
                # NMEA payload without preamble and checksum
                payload = line[1:-3]

                # verify checksum
                cksum = 0
                for char in payload:
                    cksum ^= ord(char)
                if f"*{cksum:02X}" != line[-3:]:
                    continue

                # parse date and time fields
                fields = payload.split(",")
                time = fields[1] # "HHMMSS.00"
                date = fields[9] # "DDMMYY"
                posix_sec = calendar.timegm((
                    2000 + int(date[4:6]), int(date[2:4]), int(date[0:2]),
                    int(time[0:2]), int(time[2:4]), int(time[4:6])
                ))
                posix_ns = posix_sec * 1_000_000_000 \
                         + int(float(time[6:]) * 1e9)

                # publish data, ignoring any errors
                data["time_ns"] = posix_ns
                mqtt_client.publish(mqtt_topic, json.dumps(data))

                # clear dict to avoid potentially publishing stale servo data in
                # the next iteration
                data.clear()

if __name__ == "__main__":
    import logging
    import sys

    logging.basicConfig(level=logging.INFO)

    try:
        asyncio.run(main(pathlib.Path(sys.argv[1])))
    except (asyncio.CancelledError, KeyboardInterrupt):
        pass
