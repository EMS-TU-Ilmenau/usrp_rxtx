#!/usr/bin/python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MIT
# (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

import asyncio

import asyncio_mqtt
from ntrip_client import NtripClient


class AsyncMqttClient(asyncio_mqtt.AsyncMqttClientLog):
    def on_reconnect_error(self, reason_code, exception):
        # suppress logging of reconnect errors
        pass


async def main():
    ntrip_client = NtripClient("http://user:passwort@ntrip-caster-address:port/path", 50.68221389, 10.93868317, 512.331)

    # configure MQTT client
    mqtt_client = AsyncMqttClient("192.0.2.1", async_connect=True,
                                  connect_delay=[0., 1., 2., 3., 5.],
                                  keepalive=5)
    mqtt_topic = f"usrp_rxtx/rtcm"

    async with mqtt_client:
        async for rtcm in ntrip_client:
            mqtt_client.publish(mqtt_topic, rtcm)


if __name__ == "__main__":
	import logging

	logging.basicConfig(level=logging.INFO)

	try:
		asyncio.run(main())
	except (asyncio.CancelledError, KeyboardInterrupt):
		pass
