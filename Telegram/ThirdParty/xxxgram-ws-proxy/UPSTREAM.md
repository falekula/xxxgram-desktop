Flowseal/tg-ws-proxy core, MIT license.

Source: https://github.com/Flowseal/tg-ws-proxy
Commit: 2085861957070eeea07990a522b29f42e5230210

Only the Python proxy core is included. The GUI, updater, tray integration and
system-wide proxy setup are excluded. `bridge.py` belongs to xxxgram and uses
an ephemeral IPv4 loopback listener with its secret provided over stdin.

Changes to upstream core: `pool.py` tries verified direct WebSocket connections
only; `raw_websocket.py` retains hostname verification for every TLS context.
Domain fronting is disabled. The bridge does not invoke upstream startup or its
remote domain-list refresh. Third-party Cloudflare fallback is disabled unless
the user explicitly enables it; when enabled it uses the pinned upstream list.
Encrypted direct TCP fallback to Telegram remains available in either mode.

Install the exact dependencies from `requirements.txt` into `venv` next to
`bridge.py`. No network requests or package installation occur at app startup.
