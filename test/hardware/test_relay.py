#!/usr/bin/env python3
"""Hardware tests for wisp-esp32 Nostr relay."""

import asyncio
import json
import os
import sys
import time
import urllib.request

try:
    import websockets
except ImportError:
    print("Install websockets: pip install websockets")
    sys.exit(1)

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


async def test_event_parsing():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            event = make_event("test event")
            response = await send_recv(ws, ["EVENT", event])
            data = json.loads(response)
            assert data[0] == "OK"
            assert data[1] == event["id"]
            assert data[2] is False
            print("PASS: EVENT parsing")
            passed += 1
    except Exception as e:
        print(f"FAIL: EVENT parsing - {e}")
        failed += 1


async def test_req_parsing():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            response = await send_recv(ws, ["REQ", "testsub", {"kinds": [99999], "limit": 10}])
            data = json.loads(response)
            assert data[0] == "EOSE"
            assert data[1] == "testsub"
            print("PASS: REQ parsing")
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
            assert data[0] == "CLOSED"
            assert data[1] == "testsub"
            print("PASS: CLOSE parsing")
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
            assert data[0] == "NOTICE"
            print("PASS: Invalid JSON returns NOTICE")
            passed += 1
    except Exception as e:
        print(f"FAIL: Invalid JSON - {e}")
        failed += 1


async def test_unknown_message():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            response = await send_recv(ws, ["UNKNOWN", "data"])
            data = json.loads(response)
            assert data[0] == "NOTICE"
            print("PASS: Unknown message returns NOTICE")
            passed += 1
    except Exception as e:
        print(f"FAIL: Unknown message - {e}")
        failed += 1


async def test_multiple_filters():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            response = await send_recv(ws, ["REQ", "multi", {"kinds": [99998]}, {"kinds": [99997]}])
            data = json.loads(response)
            assert data[0] == "EOSE"
            assert data[1] == "multi"
            print("PASS: Multiple filters")
            passed += 1
    except Exception as e:
        print(f"FAIL: Multiple filters - {e}")
        failed += 1


async def test_sub_id_replacement():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            await ws.send(json.dumps(["REQ", "replace_test", {"kinds": [99996]}]))
            r1 = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert r1[0] == "EOSE"

            await ws.send(json.dumps(["REQ", "replace_test", {"kinds": [99995]}]))
            r2 = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert r2[0] == "EOSE"
            assert r2[1] == "replace_test"

            await ws.send(json.dumps(["CLOSE", "replace_test"]))
            r3 = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert r3[0] == "CLOSED"
            print("PASS: Sub ID replacement")
            passed += 1
    except Exception as e:
        print(f"FAIL: Sub ID replacement - {e}")
        failed += 1


async def test_max_subscriptions():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            for i in range(8):
                await ws.send(json.dumps(["REQ", f"sub{i}", {"kinds": [99990 + i]}]))
                r = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
                assert r[0] == "EOSE"

            await ws.send(json.dumps(["REQ", "sub8", {"kinds": [99999]}]))
            r = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert r[0] == "CLOSED"

            for i in range(8):
                await ws.send(json.dumps(["CLOSE", f"sub{i}"]))
                await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)

            print("PASS: Max subscriptions (8)")
            passed += 1
    except Exception as e:
        print(f"FAIL: Max subscriptions - {e}")
        failed += 1


async def test_multiple_connections():
    global passed, failed
    try:
        ws1 = await websockets.connect(RELAY_URL)
        await asyncio.sleep(CONNECTION_DELAY)
        ws2 = await websockets.connect(RELAY_URL)
        await asyncio.sleep(CONNECTION_DELAY)
        ws3 = await websockets.connect(RELAY_URL)

        await ws1.send(json.dumps(["REQ", "conn1", {"kinds": [99981], "limit": 1}]))
        await ws2.send(json.dumps(["REQ", "conn2", {"kinds": [99982], "limit": 1}]))
        await ws3.send(json.dumps(["REQ", "conn3", {"kinds": [99983], "limit": 1}]))

        r1 = json.loads(await asyncio.wait_for(ws1.recv(), timeout=TIMEOUT))
        r2 = json.loads(await asyncio.wait_for(ws2.recv(), timeout=TIMEOUT))
        r3 = json.loads(await asyncio.wait_for(ws3.recv(), timeout=TIMEOUT))

        assert r1[0] == "EOSE" and r1[1] == "conn1"
        assert r2[0] == "EOSE" and r2[1] == "conn2"
        assert r3[0] == "EOSE" and r3[1] == "conn3"

        await close_ws(ws1)
        await close_ws(ws2)
        await close_ws(ws3)
        await asyncio.sleep(CONNECTION_DELAY)

        print("PASS: Multiple connections")
        passed += 1
    except Exception as e:
        print(f"FAIL: Multiple connections - {e}")
        failed += 1


async def test_broadcaster_fanout():
    global passed, failed, skipped
    if not HAS_PYNOSTR:
        print("SKIP: Broadcaster fan-out")
        skipped += 1
        return

    try:
        ws1 = await websockets.connect(RELAY_URL)
        await asyncio.sleep(CONNECTION_DELAY)
        ws2 = await websockets.connect(RELAY_URL)

        await ws1.send(json.dumps(["REQ", "sub1", {"kinds": [1]}]))
        while True:
            msg = json.loads(await asyncio.wait_for(ws1.recv(), timeout=TIMEOUT))
            if msg[0] == "EOSE":
                break

        event = make_signed_event("fanout test", kind=1)
        await ws2.send(json.dumps(["EVENT", event]))
        ok_data = json.loads(await asyncio.wait_for(ws2.recv(), timeout=TIMEOUT))
        assert ok_data[0] == "OK"
        assert ok_data[2] is True

        broadcast = json.loads(await asyncio.wait_for(ws1.recv(), timeout=TIMEOUT))
        assert broadcast[0] == "EVENT"
        assert broadcast[1] == "sub1"
        assert broadcast[2]["content"] == "fanout test"

        await close_ws(ws1)
        await close_ws(ws2)
        await asyncio.sleep(CONNECTION_DELAY)

        print("PASS: Broadcaster fan-out")
        passed += 1
    except Exception as e:
        print(f"FAIL: Broadcaster fan-out - {e}")
        failed += 1


async def test_multiple_subscribers():
    global passed, failed, skipped
    if not HAS_PYNOSTR:
        print("SKIP: Multiple subscribers")
        skipped += 1
        return

    try:
        ws1 = await websockets.connect(RELAY_URL)
        await asyncio.sleep(CONNECTION_DELAY)
        ws2 = await websockets.connect(RELAY_URL)
        await asyncio.sleep(CONNECTION_DELAY)
        ws3 = await websockets.connect(RELAY_URL)

        await ws1.send(json.dumps(["REQ", "client1", {"kinds": [1]}]))
        while True:
            msg = json.loads(await asyncio.wait_for(ws1.recv(), timeout=TIMEOUT))
            if msg[0] == "EOSE":
                break

        await ws2.send(json.dumps(["REQ", "client2", {"kinds": [1]}]))
        while True:
            msg = json.loads(await asyncio.wait_for(ws2.recv(), timeout=TIMEOUT))
            if msg[0] == "EOSE":
                break

        event = make_signed_event("multi subscriber test", kind=1)
        await ws3.send(json.dumps(["EVENT", event]))
        ok = json.loads(await asyncio.wait_for(ws3.recv(), timeout=TIMEOUT))
        assert ok[2] is True

        msg1 = json.loads(await asyncio.wait_for(ws1.recv(), timeout=TIMEOUT))
        msg2 = json.loads(await asyncio.wait_for(ws2.recv(), timeout=TIMEOUT))

        assert msg1[0] == "EVENT" and msg1[1] == "client1"
        assert msg2[0] == "EVENT" and msg2[1] == "client2"

        await close_ws(ws1)
        await close_ws(ws2)
        await close_ws(ws3)
        await asyncio.sleep(CONNECTION_DELAY)

        print("PASS: Multiple subscribers")
        passed += 1
    except Exception as e:
        print(f"FAIL: Multiple subscribers - {e}")
        failed += 1


async def test_ephemeral_events():
    global passed, failed, skipped
    if not HAS_PYNOSTR:
        print("SKIP: Ephemeral events")
        skipped += 1
        return

    try:
        ws1 = await websockets.connect(RELAY_URL)
        await asyncio.sleep(CONNECTION_DELAY)
        ws2 = await websockets.connect(RELAY_URL)

        await ws1.send(json.dumps(["REQ", "ephsub", {"kinds": [20001]}]))
        await asyncio.wait_for(ws1.recv(), timeout=TIMEOUT)

        event = make_signed_event("ephemeral test", kind=20001)
        await ws2.send(json.dumps(["EVENT", event]))
        ok = json.loads(await asyncio.wait_for(ws2.recv(), timeout=TIMEOUT))
        assert ok[0] == "OK"
        assert ok[2] is True

        broadcast = json.loads(await asyncio.wait_for(ws1.recv(), timeout=TIMEOUT))
        assert broadcast[0] == "EVENT"

        await close_ws(ws1)
        await close_ws(ws2)
        await asyncio.sleep(CONNECTION_DELAY)

        async with websockets.connect(RELAY_URL) as ws:
            await ws.send(json.dumps(["REQ", "check", {"kinds": [20001]}]))
            response = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert response[0] == "EOSE"
            print("PASS: Ephemeral events")
            passed += 1
    except Exception as e:
        print(f"FAIL: Ephemeral events - {e}")
        failed += 1


async def test_stored_events_before_eose():
    global passed, failed, skipped
    if not HAS_PYNOSTR:
        print("SKIP: Stored events before EOSE")
        skipped += 1
        return

    TEST_KIND = 54321
    try:
        ws = await websockets.connect(RELAY_URL)
        event = make_signed_event(f"stored_test_{int(time.time())}", kind=TEST_KIND)
        await ws.send(json.dumps(["EVENT", event]))
        ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
        await close_ws(ws)

        if ok[2] is not True:
            raise AssertionError(f"Event rejected: {get_ok_error(ok)}")

        await asyncio.sleep(CONNECTION_DELAY)

        ws = await websockets.connect(RELAY_URL)
        await ws.send(json.dumps(["REQ", "storedsub", {"kinds": [TEST_KIND], "limit": 10}]))

        messages = []
        while True:
            msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            messages.append(msg)
            if msg[0] == "EOSE":
                break

        await close_ws(ws)

        event_count = sum(1 for m in messages if m[0] == "EVENT")
        assert event_count > 0
        assert messages[-1][0] == "EOSE"
        print("PASS: Stored events before EOSE")
        passed += 1
    except Exception as e:
        print(f"FAIL: Stored events before EOSE - {e}")
        failed += 1


async def test_reject_future_event():
    global passed, failed, skipped
    if not HAS_PYNOSTR:
        print("SKIP: Reject future event")
        skipped += 1
        return

    try:
        async with websockets.connect(RELAY_URL) as ws:
            future_time = int(time.time()) + FUTURE_THRESHOLD
            event = make_signed_event("future event", kind=1, created_at=future_time)
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is False
            assert "future" in ok[3].lower()
            print("PASS: Future event rejected")
            passed += 1
    except Exception as e:
        print(f"FAIL: Reject future event - {e}")
        failed += 1


async def test_reject_expired_nip40():
    global passed, failed, skipped
    if not HAS_PYNOSTR:
        print("SKIP: Reject expired NIP-40")
        skipped += 1
        return

    try:
        async with websockets.connect(RELAY_URL) as ws:
            past_time = int(time.time()) - ONE_HOUR
            event = make_signed_event("expired event", kind=1, tags=[["expiration", str(past_time)]])
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is False
            assert "expir" in ok[3].lower()
            print("PASS: Expired NIP-40 rejected")
            passed += 1
    except Exception as e:
        print(f"FAIL: Reject expired NIP-40 - {e}")
        failed += 1


async def test_accept_valid_nip40():
    global passed, failed, skipped
    if not HAS_PYNOSTR:
        print("SKIP: Accept valid NIP-40")
        skipped += 1
        return

    try:
        async with websockets.connect(RELAY_URL) as ws:
            future_time = int(time.time()) + ONE_HOUR
            event = make_signed_event("valid expiration", kind=1, tags=[["expiration", str(future_time)]])
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is True
            print("PASS: Valid NIP-40 accepted")
            passed += 1
    except Exception as e:
        print(f"FAIL: Accept valid NIP-40 - {e}")
        failed += 1


async def test_reject_old_event():
    global passed, failed, skipped
    if not HAS_PYNOSTR:
        print("SKIP: Reject old event")
        skipped += 1
        return

    try:
        async with websockets.connect(RELAY_URL) as ws:
            old_time = int(time.time()) - MAX_EVENT_AGE
            event = make_signed_event("old event", kind=1, created_at=old_time)
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is False
            print("PASS: Old event rejected")
            passed += 1
    except Exception as e:
        print(f"FAIL: Reject old event - {e}")
        failed += 1


async def test_rate_limit_events():
    global passed, failed, skipped
    if not HAS_PYNOSTR:
        print("SKIP: Rate limit events")
        skipped += 1
        return

    try:
        ws = await websockets.connect(RELAY_URL)
        rate_limited = False
        for i in range(RATE_LIMIT_EVENTS + 1):
            event = make_signed_event(f"rate test {i}", kind=1)
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            if ok[2] is False and "rate" in ok[3].lower():
                rate_limited = True
                break
            await asyncio.sleep(0.15)

        await close_ws(ws)
        await asyncio.sleep(3.0)

        assert rate_limited
        print(f"PASS: Event rate limiting ({RATE_LIMIT_EVENTS}/min)")
        passed += 1
    except Exception as e:
        print(f"FAIL: Rate limit events - {e}")
        failed += 1


async def test_rate_limit_reqs():
    global passed, failed
    try:
        ws = await websockets.connect(RELAY_URL)
        rate_limited = False

        for i in range(RATE_LIMIT_REQS + 5):
            await ws.send(json.dumps(["REQ", f"sub{i}", {"kinds": [99999]}]))
            resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            if resp[0] == "CLOSED" and "rate" in resp[2].lower():
                rate_limited = True
                break
            await asyncio.sleep(0.1)

        await close_ws(ws)
        await asyncio.sleep(3.0)

        assert rate_limited
        print(f"PASS: REQ rate limiting ({RATE_LIMIT_REQS}/min)")
        passed += 1
    except Exception as e:
        print(f"FAIL: Rate limit REQs - {e}")
        failed += 1


async def test_rate_limit_reset_on_disconnect():
    global passed, failed, skipped
    if not HAS_PYNOSTR:
        print("SKIP: Rate limit reset")
        skipped += 1
        return

    try:
        ws = await websockets.connect(RELAY_URL)
        for i in range(RATE_LIMIT_EVENTS + 1):
            event = make_signed_event(f"rate reset test {i}", kind=1)
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            if ok[2] is False and "rate" in ok[3].lower():
                break
            await asyncio.sleep(0.15)
        await close_ws(ws)

        await asyncio.sleep(3.0)

        ws = await websockets.connect(RELAY_URL)
        event = make_signed_event("after reconnect", kind=1)
        await ws.send(json.dumps(["EVENT", event]))
        ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
        await close_ws(ws)

        assert ok[2] is True
        print("PASS: Rate limit resets on disconnect")
        passed += 1
    except Exception as e:
        print(f"FAIL: Rate limit reset - {e}")
        failed += 1


def test_nip11_json_response():
    global passed, failed
    http_url = RELAY_URL.replace("ws://", "http://").replace("wss://", "https://")

    try:
        req = urllib.request.Request(http_url)
        with urllib.request.urlopen(req, timeout=5) as resp:
            content_type = resp.headers.get("Content-Type", "")
            data = json.loads(resp.read().decode())

            assert "application/json" in content_type
            assert "name" in data
            assert "supported_nips" in data
            assert "limitation" in data
            assert "max_subscriptions" in data["limitation"]

            print("PASS: NIP-11 JSON response")
            passed += 1
    except Exception as e:
        print(f"FAIL: NIP-11 JSON response - {e}")
        failed += 1


def test_nip11_accept_header():
    global passed, failed
    http_url = RELAY_URL.replace("ws://", "http://").replace("wss://", "https://")

    try:
        req = urllib.request.Request(http_url)
        req.add_header("Accept", "application/nostr+json")
        with urllib.request.urlopen(req, timeout=5) as resp:
            content_type = resp.headers.get("Content-Type", "")
            assert "application/nostr+json" in content_type
            print("PASS: NIP-11 Accept header")
            passed += 1
    except Exception as e:
        print(f"FAIL: NIP-11 Accept header - {e}")
        failed += 1


def test_nip11_cors_headers():
    global passed, failed
    http_url = RELAY_URL.replace("ws://", "http://").replace("wss://", "https://")

    try:
        req = urllib.request.Request(http_url)
        with urllib.request.urlopen(req, timeout=5) as resp:
            cors_origin = resp.headers.get("Access-Control-Allow-Origin", "")
            cors_headers = resp.headers.get("Access-Control-Allow-Headers", "")
            cors_methods = resp.headers.get("Access-Control-Allow-Methods", "")

            assert cors_origin == "*"
            assert "Accept" in cors_headers
            assert "GET" in cors_methods

            print("PASS: NIP-11 CORS headers")
            passed += 1
    except Exception as e:
        print(f"FAIL: NIP-11 CORS headers - {e}")
        failed += 1


async def main():
    print(f"=== Relay Hardware Tests ===\nRelay: {RELAY_URL}\n")

    tests = [
        (test_event_parsing, True, 3.0),
        (test_req_parsing, True, 3.0),
        (test_close_parsing, True, 3.0),
        (test_invalid_json, True, 3.0),
        (test_unknown_message, True, 3.0),
        (test_multiple_filters, True, 3.0),
        (test_nip11_json_response, False, 2.0),
        (test_nip11_accept_header, False, 2.0),
        (test_nip11_cors_headers, False, 2.0),
        (test_reject_future_event, True, 5.0),
        (test_reject_expired_nip40, True, 5.0),
        (test_accept_valid_nip40, True, 5.0),
        (test_reject_old_event, True, 5.0),
        (test_rate_limit_events, True, 8.0),
        (test_rate_limit_reqs, True, 8.0),
        (test_rate_limit_reset_on_disconnect, True, 8.0),
        (test_broadcaster_fanout, True, 5.0),
        (test_multiple_subscribers, True, 5.0),
        (test_ephemeral_events, True, 5.0),
        (test_stored_events_before_eose, True, 5.0),
        (test_multiple_connections, True, 5.0),
        (test_sub_id_replacement, True, 5.0),
        (test_max_subscriptions, True, 5.0),
    ]

    for test, is_async, delay in tests:
        if is_async:
            await test()
        else:
            test()
        await asyncio.sleep(delay)

    print(f"\n=== Results ===\nPassed: {passed}\nFailed: {failed}")
    if skipped > 0:
        print(f"Skipped: {skipped}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
