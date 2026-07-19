#!/usr/bin/env python3
# A real MQTT 3.1.1 broker for the AeroEdge MqttClientTransport gate (014 §5). Launched by the C++ test
# via `uv run --with amqtt tests/py/mqtt_broker.py <port>`; the C++ side picks a free port, hands it here,
# waits for "BROKER_READY", runs two C++ transports through this broker, then kills the process.
#
# This is a genuine external broker (amqtt) — AeroEdge is a CLIENT to it and never reimplements one
# (014 §4 B1 / X3). The MQTT wire work under test lives entirely in the C++ client; this just brokers.
import asyncio
import logging
import sys

from amqtt.broker import Broker


async def main(port: int) -> None:
    config = {
        "listeners": {
            "default": {"type": "tcp", "bind": f"127.0.0.1:{port}"},
        },
        "sys_interval": 0,
        "auth": {"allow-anonymous": True},
    }
    broker = Broker(config)
    await broker.start()
    # The C++ harness waits on this exact line before dialing the broker.
    print(f"BROKER_READY {port}", flush=True)
    while True:
        await asyncio.sleep(3600)


if __name__ == "__main__":
    logging.basicConfig(level=logging.CRITICAL)
    try:
        asyncio.run(main(int(sys.argv[1])))
    except KeyboardInterrupt:
        pass
