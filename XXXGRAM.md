# xxxgram for Linux

Personal changes on top of official Telegram Desktop **v7.3.3**, commit
`5c674f1f6a0f6ace55b67f8bc295138680fe588b`. This checkout is based directly on
`telegramdesktop/tdesktop`, not an AyuGram source tree.

The application uses a separate desktop ID and data directory. Official Telegram
accounts and installation are not migrated or replaced. Official auto-updates
are disabled so they cannot silently replace this build.

## Defaults

- Typing, read and story-view receipts are suppressed. Offline status corrections
  and automatic scheduled sending reduce online signals. Telegram controls the
  final status: callbacks, calls, reactions and other implicit activity can still
  reveal presence. A historical last-seen timestamp cannot be frozen by a client.
- Normal outgoing messages wait 15 seconds. Settings → xxxgram allows 15–300 seconds.
  Explicit scheduled dates remain unchanged. A stale automatic request fails
  instead of silently becoming an immediate send.
- Local history covers human private chats. Defaults: 500 messages, 4 MB;
  configurable up to 10,000 messages and 64 MB per account. Old records are evicted
  at either limit. Messages never received by this client cannot be recovered.
- Deleted records remain inline; previous text versions and media-change notices
  are available in the message menu. Downloaded timed media shares the same cache
  budget. Clearing history removes these local retained copies.
- The client-side account quota is removed. Telegram's authentication and service
  limits still apply.
- Flowseal's WS proxy runs as an application-owned local process. Its third-party
  fallback is disabled by default. TLS hostname verification remains enabled.

## Proxy source

The proxy's pinned source, MIT license and integration notes are in
`Telegram/ThirdParty/xxxgram-ws-proxy`. The desktop retains its upstream license;
see `LICENSE` and `LEGAL`. AyuGram was consulted as an implementation reference.

## Build and installation

The Linux workflow in `falekula/Telegram-iOS` checks out a complete commit SHA
from this repository. It builds dependencies from that commit's official Docker
recipe and compiles the application in Debug configuration with debug symbols
omitted. API credentials enter only the application build stage. Downloadable
artifacts are AES-GCM encrypted using the existing private artifact key.

The unpacked package includes `install.sh`, the client, the proxy and pinned
Python wheels. Installation is per-user and uses a separate data directory.
Python 3.12 and either uv or the standard venv module are required. No root access
or system proxy changes are needed.
