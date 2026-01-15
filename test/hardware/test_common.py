#!/usr/bin/env python3
"""Shared utilities for hardware tests."""

import asyncio
import json
import os
import time

try:
    import websockets
except ImportError:
    websockets = None

try:
    from pynostr.key import PrivateKey
    from pynostr.event import Event as PynostrEvent
    HAS_PYNOSTR = True
except ImportError:
    HAS_PYNOSTR = False

RELAY_URL = os.environ.get("RELAY_URL", "ws://localhost:4869")
TIMEOUT = float(os.environ.get("TIMEOUT", "5"))
CLOSE_TIMEOUT = 2
CONNECTION_DELAY = 1.0

ONE_HOUR = 3600
ONE_DAY = 24 * ONE_HOUR
FUTURE_THRESHOLD = 16 * 60
MAX_EVENT_AGE = 22 * ONE_DAY
RATE_LIMIT_EVENTS = 30
RATE_LIMIT_REQS = 60

passed = 0
failed = 0
skipped = 0


async def close_ws(ws):
    try:
        await asyncio.wait_for(ws.close(), timeout=CLOSE_TIMEOUT)
    except asyncio.TimeoutError:
        pass


def get_ok_error(ok_response):
    return ok_response[3] if len(ok_response) > 3 else ""


def make_event(content="test", kind=1):
    return {
        "id": "0" * 64,
        "pubkey": "0" * 64,
        "created_at": int(time.time()),
        "kind": kind,
        "tags": [],
        "content": content,
        "sig": "0" * 128,
    }


def make_signed_event(content="test", kind=1, created_at=None, tags=None):
    if not HAS_PYNOSTR:
        return None
    privkey = PrivateKey()
    event = PynostrEvent(kind=kind, content=content)
    if created_at is not None:
        event.created_at = created_at
    if tags:
        event.tags = tags
    event.sign(privkey.hex())
    return event.to_dict()


async def send_recv(ws, msg):
    await ws.send(json.dumps(msg))
    return await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)


def print_results():
    print(f"\n=== Results ===\nPassed: {passed}\nFailed: {failed}")
    if skipped > 0:
        print(f"Skipped: {skipped}")
    return 0 if failed == 0 else 1
