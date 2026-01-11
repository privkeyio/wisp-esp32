#!/usr/bin/env python3
"""
Hardware tests for wisp-esp32 Nostr relay.

Usage:
    RELAY_URL=ws://192.168.1.100:4869 python test_relay.py
"""

import asyncio
import json
import os
import sys
import time

try:
    import websockets
except ImportError:
    print("Install websockets: pip install websockets")
    sys.exit(1)

RELAY_URL = os.environ.get("RELAY_URL", "ws://localhost:4869")
TIMEOUT = float(os.environ.get("TIMEOUT", "5"))

passed = 0
failed = 0


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


async def send_recv(ws, msg):
    await ws.send(json.dumps(msg))
    return await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)


async def test_event_parsing():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            event = make_event("test event")
            response = await send_recv(ws, ["EVENT", event])
            data = json.loads(response)

            assert data[0] == "OK", f"Expected OK, got {data[0]}"
            assert data[1] == event["id"], "Event ID mismatch"
            assert data[2] is False, "Should reject invalid signature"
            print("PASS: EVENT parsing and OK response")
            passed += 1
    except Exception as e:
        print(f"FAIL: EVENT parsing - {e}")
        failed += 1


async def test_req_parsing():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            response = await send_recv(ws, ["REQ", "testsub", {"kinds": [1], "limit": 10}])
            data = json.loads(response)

            assert data[0] == "EOSE", f"Expected EOSE, got {data[0]}"
            assert data[1] == "testsub", "Subscription ID mismatch"
            print("PASS: REQ parsing and EOSE response")
            passed += 1
    except Exception as e:
        print(f"FAIL: REQ parsing - {e}")
        failed += 1


async def test_close_parsing():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            response = await send_recv(ws, ["CLOSE", "testsub"])
            data = json.loads(response)

            assert data[0] == "CLOSED", f"Expected CLOSED, got {data[0]}"
            assert data[1] == "testsub", "Subscription ID mismatch"
            print("PASS: CLOSE parsing and CLOSED response")
            passed += 1
    except Exception as e:
        print(f"FAIL: CLOSE parsing - {e}")
        failed += 1


async def test_invalid_json():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            await ws.send("not valid json")
            response = await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)
            data = json.loads(response)

            assert data[0] == "NOTICE", f"Expected NOTICE, got {data[0]}"
            print("PASS: Invalid JSON returns NOTICE")
            passed += 1
    except Exception as e:
        print(f"FAIL: Invalid JSON handling - {e}")
        failed += 1


async def test_unknown_message():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            response = await send_recv(ws, ["UNKNOWN", "data"])
            data = json.loads(response)

            assert data[0] == "NOTICE", f"Expected NOTICE, got {data[0]}"
            print("PASS: Unknown message type returns NOTICE")
            passed += 1
    except Exception as e:
        print(f"FAIL: Unknown message handling - {e}")
        failed += 1


async def test_multiple_filters():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            response = await send_recv(ws, ["REQ", "multi", {"kinds": [1]}, {"kinds": [0]}])
            data = json.loads(response)

            assert data[0] == "EOSE", f"Expected EOSE, got {data[0]}"
            assert data[1] == "multi", "Subscription ID mismatch"
            print("PASS: REQ with multiple filters")
            passed += 1
    except Exception as e:
        print(f"FAIL: Multiple filters - {e}")
        failed += 1


async def main():
    print("=== Relay Hardware Tests ===")
    print(f"Relay: {RELAY_URL}")
    print()

    await test_event_parsing()
    await test_req_parsing()
    await test_close_parsing()
    await test_invalid_json()
    await test_unknown_message()
    await test_multiple_filters()

    print()
    print("=== Results ===")
    print(f"Passed: {passed}")
    print(f"Failed: {failed}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
