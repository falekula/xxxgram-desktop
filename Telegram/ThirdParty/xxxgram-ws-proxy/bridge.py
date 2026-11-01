"""Private, loopback-only process bridge for xxxgram; configuration arrives on stdin."""
import asyncio
import json
import logging
import os
import re
import signal
import sys

logging.disable(logging.CRITICAL)

from proxy import tg_ws_proxy as core
from proxy.balancer import balancer
from proxy.config import CFPROXY_DEFAULT_DOMAINS, proxy_config


def parse_configuration(line):
    if len(line) > 4096 or not line.endswith(b'\n'):
        raise ValueError('invalid_configuration')
    value = json.loads(line)
    if not isinstance(value, dict) or set(value) != {'secret', 'fallback'}:
        raise ValueError('invalid_configuration')
    if not isinstance(value['secret'], str) or not re.fullmatch('[0-9a-f]{32}', value['secret']):
        raise ValueError('invalid_configuration')
    if type(value['fallback']) is not bool:
        raise ValueError('invalid_configuration')
    return value


def configure(value):
    proxy_config.host = '127.0.0.1'
    proxy_config.port = 0
    proxy_config.secret = value['secret']
    proxy_config.fallback_cfproxy = value['fallback']
    proxy_config.cfproxy_h2_media = value['fallback']
    proxy_config.cfproxy_user_domains = []
    proxy_config.cfproxy_worker_domains = []
    proxy_config.disable_secure = False
    proxy_config.fake_tls_domain = ''
    proxy_config.proxy_protocol = False
    proxy_config.force_test_dc = False
    balancer.update_domains_list(CFPROXY_DEFAULT_DOMAINS if value['fallback'] else [])


def emit(value):
    print(json.dumps(value, separators=(',', ':')), flush=True)


async def run(value, stop, ready=emit):
    configure(value)
    core.reset_tcp_backoff()
    core.ws_pool.reset()
    core.cf_worker_pool.reset()
    core.cf_h2_pool = core.CfH2Pool() if proxy_config.h2_enabled else None
    clients = set()
    secret = bytes.fromhex(value['secret'])

    def accept(reader, writer):
        if stop.is_set() or len(clients) >= 128:
            writer.close()
            return
        task = asyncio.create_task(core._handle_client(reader, writer, secret))
        clients.add(task)
        task.add_done_callback(clients.discard)

    server = await asyncio.start_server(accept, '127.0.0.1', 0)
    try:
        port = server.sockets[0].getsockname()[1]
        ready({'state': 'ready', 'port': port})
        await core.ws_pool.warmup()
        await stop.wait()
    finally:
        server.close()
        await server.wait_closed()
        pending = list(clients)
        for task in pending:
            task.cancel()
        await asyncio.gather(*pending, return_exceptions=True)
        await core.ws_pool.close()
        await core.cf_worker_pool.close()
        if core.cf_h2_pool is not None:
            await core.cf_h2_pool.close()
            core.cf_h2_pool = None


async def main_async(value):
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    descriptor = sys.stdin.fileno()

    def input_ready():
        if not os.read(descriptor, 4096):
            stop.set()

    loop.add_reader(descriptor, input_ready)
    for signum in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(signum, stop.set)
    try:
        await run(value, stop)
    finally:
        loop.remove_reader(descriptor)


def main():
    try:
        value = parse_configuration(sys.stdin.buffer.readline(4097))
        asyncio.run(main_async(value))
    except Exception:
        emit({'state': 'error', 'reason': 'proxy_unavailable'})
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
