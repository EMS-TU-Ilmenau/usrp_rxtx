#!/usr/bin/python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MIT
# (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

import asyncio
import asyncio_mqtt
import json
import logging
import os
import pathlib
import signal
import socket
import subprocess
import time
from typing import Self

_logger = logging.getLogger(__name__)

class AsyncMqttClient(asyncio_mqtt.AsyncMqttClientLog):
    def on_reconnect_error(self, reason_code, exception):
        # suppress logging of reconnect errors
        pass


async def main():
    hostname = socket.gethostname()
    loop = asyncio.get_running_loop()

    # setup graceful termination by cancelling tasks on SIGINT/SIGTERM
    if os.name == "posix":
        task = asyncio.current_task()
        loop.add_signal_handler(signal.SIGINT,  lambda: task.cancel())
        loop.add_signal_handler(signal.SIGTERM, lambda: task.cancel())

    loadavg = open("/proc/loadavg", "rb", buffering=0)

    # identify hwmon files for CPU and SSD temperatures
    temps_cpu = []
    temps_ssd = []
    for hwmon in pathlib.Path("/sys/class/hwmon").glob("hwmon*"):
        try:
            name = (hwmon / "name").read_text().rstrip("\n")
        except FileNotFoundError:
            continue

        if name in {"coretemp", "cpu_thermal", "k10temp"}:
            for temp_path in hwmon.glob("temp*_input"):
                temps_cpu.append(open(temp_path, "rb", buffering=0))
        elif name in {"nvme"}:
            for temp_path in hwmon.glob("temp*_input"):
                temps_ssd.append(open(temp_path, "rb", buffering=0))

    # configure MQTT client
    mqtt_client = AsyncMqttClient("192.0.2.1", async_connect=True,
                                  connect_delay=[0., 1., 2., 3., 5.],
                                  keepalive=5)
    mqtt_topic = f"usrp_rxtx/hwmon/{socket.gethostname()}"

    async with mqtt_client:
        maxtemp_cpu = None
        maxtemp_ssd = None

        while True:
            # align to next full second
            now = time.time()
            await asyncio.sleep(1 + int(now) - now)

            if temps_cpu:
                maxtemp_cpu = round(1e-3 * max((int(temp.read(64)) for temp in temps_cpu)))
            if temps_ssd:
                maxtemp_ssd = round(1e-3 * max((int(temp.read(64)) for temp in temps_ssd)))

            statvfs = os.statvfs("/")

            data = {
                "time_ns": None,
                "system_state": None,
                "load": float(loadavg.read(64).decode().split(" ")[0]),
                "temp_cpu": maxtemp_cpu,
                "temp_ssd": maxtemp_ssd,
                "disk_avail": statvfs.f_bsize * statvfs.f_bavail
            }

            # read systemd SystemState (e.g., running or degraded)
            sysctl = subprocess.run(["systemctl", "show"], capture_output=True, timeout=0.1)
            for line in sysctl.stdout.decode().split("\n"):
                if line.startswith("SystemState="):
                    data["system_state"] = line.removeprefix("SystemState=")

            # publish data, ignoring any errors
            data["time_ns"] = time.time_ns()
            mqtt_client.publish(mqtt_topic, json.dumps(data))

            # rewind all files
            loadavg.seek(0)
            for file in temps_cpu:
                file.seek(0)
            for file in temps_ssd:
                file.seek(0)

if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)

    try:
        asyncio.run(main())
    except (asyncio.CancelledError, KeyboardInterrupt):
        pass
