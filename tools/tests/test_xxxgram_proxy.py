import asyncio
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import ssl
import sys
import unittest
from unittest.mock import AsyncMock, patch


ROOT = Path(__file__).resolve().parents[2]
PAYLOAD = ROOT / 'Telegram/ThirdParty/xxxgram-ws-proxy'
sys.path.insert(0, str(PAYLOAD))
spec = importlib.util.spec_from_file_location('xxxgram_bridge', PAYLOAD / 'bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)

from proxy import raw_websocket
from proxy.config import proxy_config
from proxy.pool import _WsPool


CONFIGURATION = {'secret': '12' * 16, 'fallback': False}


class ConfigurationTests(unittest.TestCase):
    def test_strict_configuration_and_disabled_third_party_defaults(self):
        value = bridge.parse_configuration(json.dumps(CONFIGURATION).encode() + b'\n')
        bridge.configure(value)
        self.assertEqual(proxy_config.host, '127.0.0.1')
        self.assertEqual(proxy_config.port, 0)
        self.assertFalse(proxy_config.fallback_cfproxy)
        self.assertFalse(proxy_config.h2_enabled)
        self.assertEqual(proxy_config.cfproxy_worker_domains, [])
        self.assertFalse(proxy_config.disable_secure)
        for invalid in [b'{}\n', b'[]\n', b'x' * 4097,
                        json.dumps({**CONFIGURATION, 'host': '0.0.0.0'}).encode() + b'\n',
                        json.dumps({**CONFIGURATION, 'fallback': 1}).encode() + b'\n',
                        json.dumps({**CONFIGURATION, 'secret': 'short'}).encode() + b'\n']:
            with self.subTest(value=invalid[:80]):
                with self.assertRaises((ValueError, json.JSONDecodeError)):
                    bridge.parse_configuration(invalid)

    def test_all_tls_contexts_verify_hostnames(self):
        for context in [raw_websocket._ssl_ctx, raw_websocket._ssl_ctx_fronting]:
            self.assertTrue(context.check_hostname)
            self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)


class LifecycleTests(unittest.IsolatedAsyncioTestCase):
    async def test_ephemeral_loopback_ready_and_shutdown_without_domain_refresh(self):
        stop = asyncio.Event()
        ready = asyncio.get_running_loop().create_future()
        with patch.object(bridge.core.ws_pool, 'warmup', new_callable=AsyncMock), \
                patch.object(bridge.core, 'start_cfproxy_domain_refresh') as refresh:
            task = asyncio.create_task(bridge.run(CONFIGURATION, stop, ready.set_result))
            value = await asyncio.wait_for(ready, timeout=2)
            self.assertEqual(set(value), {'state', 'port'})
            self.assertEqual(value['state'], 'ready')
            self.assertGreater(value['port'], 1023)
            reader, writer = await asyncio.open_connection('127.0.0.1', value['port'])
            writer.close()
            await writer.wait_closed()
            stop.set()
            await asyncio.wait_for(task, timeout=2)
            with self.assertRaises(OSError):
                await asyncio.open_connection('127.0.0.1', value['port'])
            refresh.assert_not_called()

    async def test_parent_pipe_eof_stops_child_without_printing_secret(self):
        read_fd, write_fd = os.pipe()
        reader = os.fdopen(read_fd, 'r')
        output = io.StringIO()
        try:
            with patch.object(bridge.sys, 'stdin', reader), \
                    contextlib.redirect_stdout(output), \
                    patch.object(bridge.core.ws_pool, 'warmup', new_callable=AsyncMock):
                task = asyncio.create_task(bridge.main_async(CONFIGURATION))
                for _ in range(100):
                    if output.getvalue():
                        break
                    await asyncio.sleep(0.01)
                self.assertIn('ready', output.getvalue())
                os.close(write_fd)
                write_fd = None
                await asyncio.wait_for(task, timeout=2)
            self.assertNotIn(CONFIGURATION['secret'], output.getvalue())
            self.assertEqual(len(output.getvalue().splitlines()), 1)
        finally:
            reader.close()
            if write_fd is not None:
                os.close(write_fd)

    async def test_ws_failure_never_tries_unverified_fronting(self):
        pool = _WsPool()
        with patch.object(raw_websocket.RawWebSocket, 'connect', new_callable=AsyncMock) as connect:
            connect.side_effect = OSError('unreachable')
            result = await pool._connect_one('149.154.167.220', ['kws4.web.telegram.org'])
        self.assertIsNone(result)
        self.assertEqual(connect.await_count, 1)
        self.assertIsNone(connect.call_args.kwargs['sni'])


if __name__ == '__main__':
    unittest.main()
