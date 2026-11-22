# SPDX-License-Identifier: AGPL-3.0-or-later
"""tlsrelay.py [--listen ADDR:PORT] HOST: plain HTTP and WebSocket in, HTTPS to HOST:443 out.

The phone has no TLS, and the public packet monitor is HTTPS only (its daemon listens on
loopback behind a Cloudflare tunnel). This relays each plain connection to HOST over TLS,
rewriting the Host header of the request that opens it, so

    python3 tools/tlsrelay.py --listen 0.0.0.0:8099 monitor.ukpacketradio.network
    native/waterfall http://<this machine>:8099/

shows the monitor's list. One upstream connection per client connection, nothing kept.
"""
import argparse
import asyncio
import re
import ssl
import sys


async def pipe(r, w):
    try:
        while data := await r.read(65536):
            w.write(data)
            await w.drain()
    except (ConnectionError, asyncio.IncompleteReadError):
        pass
    finally:
        w.close()


async def client(cr, cw, host, ctx):
    try:
        head = await asyncio.wait_for(cr.readuntil(b"\r\n\r\n"), 10)
    except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
        cw.close()
        return
    head = re.sub(rb"(?im)^host:[^\r\n]*", b"Host: " + host.encode(), head, count=1)
    try:
        ur, uw = await asyncio.wait_for(asyncio.open_connection(host, 443, ssl=ctx, server_hostname=host), 10)
    except (OSError, asyncio.TimeoutError) as e:
        print(f"cannot reach {host}: {e}", file=sys.stderr)
        cw.close()
        return
    uw.write(head)
    await asyncio.gather(pipe(cr, uw), pipe(ur, cw))


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="127.0.0.1:8099")
    ap.add_argument("host")
    a = ap.parse_args()
    addr, port = a.listen.rsplit(":", 1)
    ctx = ssl.create_default_context()
    srv = await asyncio.start_server(lambda r, w: client(r, w, a.host, ctx), addr, int(port))
    print(f"relaying {a.listen} -> https://{a.host}/", file=sys.stderr)
    async with srv:
        await srv.serve_forever()


asyncio.run(main())
