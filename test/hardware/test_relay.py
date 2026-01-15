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

try:
    from pynostr.key import PrivateKey
    from pynostr.event import Event as PynostrEvent
    HAS_PYNOSTR = True
except ImportError:
    HAS_PYNOSTR = False

RELAY_URL = os.environ.get("RELAY_URL", "ws://localhost:4869")
TIMEOUT = float(os.environ.get("TIMEOUT", "5"))
CLOSE_TIMEOUT = 2  # WebSocket close timeout
CONNECTION_DELAY = 1.0  # Delay between connections to avoid overwhelming relay

# Time constants for TTL tests (in seconds)
ONE_HOUR = 3600
ONE_DAY = 24 * ONE_HOUR
FUTURE_THRESHOLD = 16 * 60  # 16 minutes (relay rejects events >15 min in future)
MAX_EVENT_AGE = 22 * ONE_DAY  # 22 days (relay rejects events >21 days old)

passed = 0
failed = 0
skipped = 0


async def close_ws(ws):
    """Close WebSocket with timeout to avoid hanging."""
    try:
        await asyncio.wait_for(ws.close(), timeout=CLOSE_TIMEOUT)
    except asyncio.TimeoutError:
        pass


def get_ok_error(ok_response):
    """Extract error message from OK response, or empty string if none."""
    return ok_response[3] if len(ok_response) > 3 else ""


def make_event(content="test", kind=1):
    """Create an unsigned/invalid event for basic parsing tests."""
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
    """Create a properly signed Nostr event using pynostr."""
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
            # Use kind 99999 which won't have stored events
            response = await send_recv(ws, ["REQ", "testsub", {"kinds": [99999], "limit": 10}])
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
            # Use kinds that won't have stored events
            response = await send_recv(ws, ["REQ", "multi", {"kinds": [99998]}, {"kinds": [99997]}])
            data = json.loads(response)

            assert data[0] == "EOSE", f"Expected EOSE, got {data[0]}"
            assert data[1] == "multi", "Subscription ID mismatch"
            print("PASS: REQ with multiple filters")
            passed += 1
    except Exception as e:
        print(f"FAIL: Multiple filters - {e}")
        failed += 1


async def test_sub_id_replacement():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            # Use kinds that won't have stored events
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
            print("PASS: Same sub_id replaces filters")
            passed += 1
    except Exception as e:
        print(f"FAIL: Sub ID replacement - {e}")
        failed += 1


async def test_max_subscriptions():
    global passed, failed
    try:
        async with websockets.connect(RELAY_URL) as ws:
            for i in range(8):
                # Use kinds that won't have stored events
                await ws.send(json.dumps(["REQ", f"sub{i}", {"kinds": [99990 + i]}]))
                r = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
                assert r[0] == "EOSE", f"Sub {i} failed: {r}"

            await ws.send(json.dumps(["REQ", "sub8", {"kinds": [99999]}]))
            r = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert r[0] == "CLOSED", f"9th sub should be rejected, got {r[0]}"

            for i in range(8):
                await ws.send(json.dumps(["CLOSE", f"sub{i}"]))
                await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)

            print("PASS: 9th subscription rejected")
            passed += 1
    except Exception as e:
        print(f"FAIL: Max subscriptions - {e}")
        failed += 1


async def test_multiple_connections():
    """Test that multiple connections can be opened and receive responses."""
    global passed, failed
    try:
        ws1 = await websockets.connect(RELAY_URL)
        await asyncio.sleep(CONNECTION_DELAY)
        ws2 = await websockets.connect(RELAY_URL)
        await asyncio.sleep(CONNECTION_DELAY)
        ws3 = await websockets.connect(RELAY_URL)

        # Use kinds that won't have stored events
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
        await asyncio.sleep(CONNECTION_DELAY)  # Wait for server cleanup

        print("PASS: Multiple simultaneous connections")
        passed += 1
    except Exception as e:
        print(f"FAIL: Multiple connections - {e}")
        failed += 1


async def test_broadcaster_fanout():
    """Test that valid events are broadcast to subscribers (requires pynostr)."""
    global passed, failed, skipped

    if not HAS_PYNOSTR:
        print("SKIP: Broadcaster fan-out (pynostr not installed)")
        skipped += 1
        return

    try:
        ws1 = await websockets.connect(RELAY_URL)
        await asyncio.sleep(CONNECTION_DELAY)
        ws2 = await websockets.connect(RELAY_URL)

        await ws1.send(json.dumps(["REQ", "sub1", {"kinds": [1]}]))
        # Drain any stored events until EOSE
        while True:
            msg = json.loads(await asyncio.wait_for(ws1.recv(), timeout=TIMEOUT))
            if msg[0] == "EOSE":
                break

        event = make_signed_event("fanout test", kind=1)
        await ws2.send(json.dumps(["EVENT", event]))
        ok_resp = await asyncio.wait_for(ws2.recv(), timeout=TIMEOUT)
        ok_data = json.loads(ok_resp)
        assert ok_data[0] == "OK"
        assert ok_data[2] is True, f"Event rejected: {get_ok_error(ok_data) or 'unknown'}"

        broadcast = await asyncio.wait_for(ws1.recv(), timeout=TIMEOUT)
        data = json.loads(broadcast)

        assert data[0] == "EVENT", f"Expected EVENT, got {data[0]}"
        assert data[1] == "sub1", f"Expected sub_id 'sub1', got {data[1]}"
        assert data[2]["content"] == "fanout test"

        await close_ws(ws1)
        await close_ws(ws2)
        await asyncio.sleep(CONNECTION_DELAY)  # Wait for server cleanup

        print("PASS: Broadcaster fan-out")
        passed += 1
    except Exception as e:
        print(f"FAIL: Broadcaster fan-out - {e}")
        failed += 1


async def test_multiple_subscribers():
    """Test that events reach multiple subscribers (requires pynostr)."""
    global passed, failed, skipped

    if not HAS_PYNOSTR:
        print("SKIP: Multiple subscribers (pynostr not installed)")
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
        assert ok[2] is True, f"Event rejected: {get_ok_error(ok) or 'unknown'}"

        msg1 = json.loads(await asyncio.wait_for(ws1.recv(), timeout=TIMEOUT))
        msg2 = json.loads(await asyncio.wait_for(ws2.recv(), timeout=TIMEOUT))

        assert msg1[0] == "EVENT" and msg1[1] == "client1"
        assert msg2[0] == "EVENT" and msg2[1] == "client2"
        assert msg1[2]["content"] == msg2[2]["content"] == "multi subscriber test"

        await close_ws(ws1)
        await close_ws(ws2)
        await close_ws(ws3)
        await asyncio.sleep(CONNECTION_DELAY)  # Wait for server cleanup

        print("PASS: Multiple subscribers receive events")
        passed += 1
    except Exception as e:
        print(f"FAIL: Multiple subscribers - {e}")
        failed += 1


async def test_ephemeral_events():
    """Test ephemeral events (requires pynostr)."""
    global passed, failed, skipped

    if not HAS_PYNOSTR:
        print("SKIP: Ephemeral events (pynostr not installed)")
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
        ok_resp = await asyncio.wait_for(ws2.recv(), timeout=TIMEOUT)
        ok = json.loads(ok_resp)
        assert ok[0] == "OK"
        assert ok[2] is True, f"Event rejected: {get_ok_error(ok) or 'unknown'}"

        broadcast = await asyncio.wait_for(ws1.recv(), timeout=TIMEOUT)
        data = json.loads(broadcast)
        assert data[0] == "EVENT", "Ephemeral event should be broadcast"

        await close_ws(ws1)
        await close_ws(ws2)
        await asyncio.sleep(CONNECTION_DELAY)  # Wait for server cleanup

        async with websockets.connect(RELAY_URL) as ws:
            await ws.send(json.dumps(["REQ", "check", {"kinds": [20001]}]))
            response = await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)
            data = json.loads(response)
            assert data[0] == "EOSE", "Ephemeral events should not be stored"
            print("PASS: Ephemeral events (broadcast but not stored)")
            passed += 1
    except Exception as e:
        print(f"FAIL: Ephemeral events - {e}")
        failed += 1


async def test_stored_events_before_eose():
    global passed, failed, skipped

    if not HAS_PYNOSTR:
        print("SKIP: Stored events before EOSE (pynostr not installed)")
        skipped += 1
        return

    try:
        async with websockets.connect(RELAY_URL) as ws:
            event = make_signed_event(f"stored_test_{int(time.time())}", kind=1)
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert ok[2] is True, f"Event rejected: {get_ok_error(ok) or 'unknown'}"

        await asyncio.sleep(0.5)

        async with websockets.connect(RELAY_URL) as ws:
            await ws.send(json.dumps(["REQ", "storedsub", {"kinds": [1], "limit": 10}]))

            messages = []
            while True:
                msg = await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)
                data = json.loads(msg)
                messages.append(data)
                if data[0] == "EOSE":
                    break

            event_count = sum(1 for m in messages if m[0] == "EVENT")
            assert event_count > 0, "Should have at least one stored event"
            assert messages[-1][0] == "EOSE", "EOSE should be last message"
            print("PASS: Stored events sent before EOSE")
            passed += 1
    except Exception as e:
        print(f"FAIL: Stored events before EOSE - {e}")
        failed += 1


async def test_reject_future_event():
    """Test that events >15 minutes in the future are rejected."""
    global passed, failed, skipped

    if not HAS_PYNOSTR:
        print("SKIP: Reject future event (pynostr not installed)")
        skipped += 1
        return

    try:
        async with websockets.connect(RELAY_URL) as ws:
            future_time = int(time.time()) + FUTURE_THRESHOLD
            event = make_signed_event("future event", kind=1, created_at=future_time)
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is False, "Future event should be rejected"
            assert "future" in ok[3].lower(), f"Expected 'future' in error: {ok[3]}"
            print("PASS: Future event rejected")
            passed += 1
    except Exception as e:
        print(f"FAIL: Reject future event - {e}")
        failed += 1


async def test_reject_expired_nip40():
    """Test that events with expired NIP-40 expiration tags are rejected."""
    global passed, failed, skipped

    if not HAS_PYNOSTR:
        print("SKIP: Reject expired NIP-40 (pynostr not installed)")
        skipped += 1
        return

    try:
        async with websockets.connect(RELAY_URL) as ws:
            past_time = int(time.time()) - ONE_HOUR
            event = make_signed_event(
                "expired event", kind=1,
                tags=[["expiration", str(past_time)]]
            )
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is False, "Expired NIP-40 event should be rejected"
            assert "expir" in ok[3].lower(), f"Expected 'expir' in error: {ok[3]}"
            print("PASS: Expired NIP-40 event rejected")
            passed += 1
    except Exception as e:
        print(f"FAIL: Reject expired NIP-40 - {e}")
        failed += 1


async def test_accept_valid_nip40():
    """Test that events with valid NIP-40 expiration tags are accepted."""
    global passed, failed, skipped

    if not HAS_PYNOSTR:
        print("SKIP: Accept valid NIP-40 (pynostr not installed)")
        skipped += 1
        return

    try:
        async with websockets.connect(RELAY_URL) as ws:
            future_time = int(time.time()) + ONE_HOUR
            event = make_signed_event(
                "valid expiration event", kind=1,
                tags=[["expiration", str(future_time)]]
            )
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is True, f"Valid NIP-40 event should be accepted: {get_ok_error(ok)}"
            print("PASS: Valid NIP-40 event accepted")
            passed += 1
    except Exception as e:
        print(f"FAIL: Accept valid NIP-40 - {e}")
        failed += 1


async def test_reject_old_event():
    """Test that events older than 21 days are rejected."""
    global passed, failed, skipped

    if not HAS_PYNOSTR:
        print("SKIP: Reject old event (pynostr not installed)")
        skipped += 1
        return

    try:
        async with websockets.connect(RELAY_URL) as ws:
            old_time = int(time.time()) - MAX_EVENT_AGE
            event = make_signed_event("old event", kind=1, created_at=old_time)
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is False, "Old event should be rejected"
            print("PASS: Old event (>21 days) rejected")
            passed += 1
    except Exception as e:
        print(f"FAIL: Reject old event - {e}")
        failed += 1


async def main():
    print("=== Relay Hardware Tests ===")
    print(f"Relay: {RELAY_URL}")
    if not HAS_PYNOSTR:
        print("Note: pynostr not installed, broadcaster tests will be skipped")
        print("      Install with: pip install pynostr")
    print()

    # Group 1: Basic tests and broadcaster tests
    tests_group1 = [
        test_event_parsing,
        test_req_parsing,
        test_close_parsing,
        test_invalid_json,
        test_unknown_message,
        test_multiple_filters,
        test_broadcaster_fanout,
        test_multiple_subscribers,
    ]

    # Group 2: Connection stress tests and storage tests
    tests_group2 = [
        test_ephemeral_events,
        test_sub_id_replacement,
        test_max_subscriptions,
        test_multiple_connections,
        test_stored_events_before_eose,
    ]

    # Group 3: TTL and validation tests
    tests_group3 = [
        test_reject_future_event,
        test_reject_expired_nip40,
        test_accept_valid_nip40,
        test_reject_old_event,
    ]

    print("--- Group 1: Basic and Broadcaster Tests ---")
    for test in tests_group1:
        await test()
        await asyncio.sleep(5.0)

    # Give relay time to recover between groups
    print("\n--- Waiting for relay to stabilize ---")
    await asyncio.sleep(5.0)

    print("\n--- Group 2: Connection and Storage Tests ---")
    for test in tests_group2:
        await test()
        await asyncio.sleep(5.0)

    print("\n--- Waiting for relay to stabilize ---")
    await asyncio.sleep(5.0)

    print("\n--- Group 3: TTL and Validation Tests ---")
    for test in tests_group3:
        await test()
        await asyncio.sleep(5.0)

    print()
    print("=== Results ===")
    print(f"Passed: {passed}")
    print(f"Failed: {failed}")
    if skipped > 0:
        print(f"Skipped: {skipped}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
