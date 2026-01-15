#!/usr/bin/env python3
"""Broadcaster, subscription, and connection tests."""

import asyncio
import json
import sys
import time

import test_common as tc

if not tc.websockets:
    print("Install websockets: pip install websockets")
    sys.exit(1)


async def test_broadcaster_fanout():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Broadcaster fan-out")
        tc.skipped += 1
        return

    try:
        ws1 = await tc.websockets.connect(tc.RELAY_URL)
        await asyncio.sleep(tc.CONNECTION_DELAY)
        ws2 = await tc.websockets.connect(tc.RELAY_URL)

        await ws1.send(json.dumps(["REQ", "sub1", {"kinds": [1]}]))
        while True:
            msg = json.loads(await asyncio.wait_for(ws1.recv(), timeout=tc.TIMEOUT))
            if msg[0] == "EOSE":
                break

        event = tc.make_signed_event("fanout test", kind=1)
        await ws2.send(json.dumps(["EVENT", event]))
        ok = json.loads(await asyncio.wait_for(ws2.recv(), timeout=tc.TIMEOUT))
        assert ok[2] is True

        broadcast = json.loads(await asyncio.wait_for(ws1.recv(), timeout=tc.TIMEOUT))
        assert broadcast[0] == "EVENT"
        assert broadcast[2]["content"] == "fanout test"

        await tc.close_ws(ws1)
        await tc.close_ws(ws2)
        await asyncio.sleep(tc.CONNECTION_DELAY)
        print("PASS: Broadcaster fan-out")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Broadcaster fan-out - {e}")
        tc.failed += 1


async def test_multiple_subscribers():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Multiple subscribers")
        tc.skipped += 1
        return

    try:
        ws1 = await tc.websockets.connect(tc.RELAY_URL)
        await asyncio.sleep(tc.CONNECTION_DELAY)
        ws2 = await tc.websockets.connect(tc.RELAY_URL)
        await asyncio.sleep(tc.CONNECTION_DELAY)
        ws3 = await tc.websockets.connect(tc.RELAY_URL)

        await ws1.send(json.dumps(["REQ", "c1", {"kinds": [1]}]))
        while json.loads(await asyncio.wait_for(ws1.recv(), timeout=tc.TIMEOUT))[0] != "EOSE":
            pass

        await ws2.send(json.dumps(["REQ", "c2", {"kinds": [1]}]))
        while json.loads(await asyncio.wait_for(ws2.recv(), timeout=tc.TIMEOUT))[0] != "EOSE":
            pass

        event = tc.make_signed_event("multi sub test", kind=1)
        await ws3.send(json.dumps(["EVENT", event]))
        ok = json.loads(await asyncio.wait_for(ws3.recv(), timeout=tc.TIMEOUT))
        assert ok[2] is True

        msg1 = json.loads(await asyncio.wait_for(ws1.recv(), timeout=tc.TIMEOUT))
        msg2 = json.loads(await asyncio.wait_for(ws2.recv(), timeout=tc.TIMEOUT))
        assert msg1[0] == "EVENT" and msg2[0] == "EVENT"

        await tc.close_ws(ws1)
        await tc.close_ws(ws2)
        await tc.close_ws(ws3)
        await asyncio.sleep(tc.CONNECTION_DELAY)
        print("PASS: Multiple subscribers")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Multiple subscribers - {e}")
        tc.failed += 1


async def test_ephemeral_events():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Ephemeral events")
        tc.skipped += 1
        return

    try:
        ws1 = await tc.websockets.connect(tc.RELAY_URL)
        await asyncio.sleep(tc.CONNECTION_DELAY)
        ws2 = await tc.websockets.connect(tc.RELAY_URL)

        await ws1.send(json.dumps(["REQ", "eph", {"kinds": [20001]}]))
        await asyncio.wait_for(ws1.recv(), timeout=tc.TIMEOUT)

        event = tc.make_signed_event("ephemeral", kind=20001)
        await ws2.send(json.dumps(["EVENT", event]))
        ok = json.loads(await asyncio.wait_for(ws2.recv(), timeout=tc.TIMEOUT))
        assert ok[2] is True

        broadcast = json.loads(await asyncio.wait_for(ws1.recv(), timeout=tc.TIMEOUT))
        assert broadcast[0] == "EVENT"

        await tc.close_ws(ws1)
        await tc.close_ws(ws2)
        await asyncio.sleep(tc.CONNECTION_DELAY)

        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            await ws.send(json.dumps(["REQ", "check", {"kinds": [20001]}]))
            resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            assert resp[0] == "EOSE"
        print("PASS: Ephemeral events")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Ephemeral events - {e}")
        tc.failed += 1


async def test_stored_events():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Stored events")
        tc.skipped += 1
        return

    TEST_KIND = 54321
    try:
        ws = await tc.websockets.connect(tc.RELAY_URL)
        event = tc.make_signed_event(f"stored_{int(time.time())}", kind=TEST_KIND)
        await ws.send(json.dumps(["EVENT", event]))
        ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
        await tc.close_ws(ws)
        assert ok[2] is True

        await asyncio.sleep(tc.CONNECTION_DELAY)

        ws = await tc.websockets.connect(tc.RELAY_URL)
        await ws.send(json.dumps(["REQ", "stored", {"kinds": [TEST_KIND], "limit": 10}]))
        messages = []
        while True:
            msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            messages.append(msg)
            if msg[0] == "EOSE":
                break
        await tc.close_ws(ws)

        assert sum(1 for m in messages if m[0] == "EVENT") > 0
        assert messages[-1][0] == "EOSE"
        print("PASS: Stored events before EOSE")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Stored events - {e}")
        tc.failed += 1


async def test_multiple_connections():
    try:
        ws1 = await tc.websockets.connect(tc.RELAY_URL)
        await asyncio.sleep(tc.CONNECTION_DELAY)
        ws2 = await tc.websockets.connect(tc.RELAY_URL)
        await asyncio.sleep(tc.CONNECTION_DELAY)
        ws3 = await tc.websockets.connect(tc.RELAY_URL)

        await ws1.send(json.dumps(["REQ", "c1", {"kinds": [99981]}]))
        await ws2.send(json.dumps(["REQ", "c2", {"kinds": [99982]}]))
        await ws3.send(json.dumps(["REQ", "c3", {"kinds": [99983]}]))

        r1 = json.loads(await asyncio.wait_for(ws1.recv(), timeout=tc.TIMEOUT))
        r2 = json.loads(await asyncio.wait_for(ws2.recv(), timeout=tc.TIMEOUT))
        r3 = json.loads(await asyncio.wait_for(ws3.recv(), timeout=tc.TIMEOUT))

        assert r1[0] == "EOSE" and r2[0] == "EOSE" and r3[0] == "EOSE"

        await tc.close_ws(ws1)
        await tc.close_ws(ws2)
        await tc.close_ws(ws3)
        await asyncio.sleep(tc.CONNECTION_DELAY)
        print("PASS: Multiple connections")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Multiple connections - {e}")
        tc.failed += 1


async def test_sub_id_replacement():
    try:
        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            await ws.send(json.dumps(["REQ", "test", {"kinds": [99996]}]))
            r1 = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            assert r1[0] == "EOSE"

            await ws.send(json.dumps(["REQ", "test", {"kinds": [99995]}]))
            r2 = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            assert r2[0] == "EOSE"

            await ws.send(json.dumps(["CLOSE", "test"]))
            r3 = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            assert r3[0] == "CLOSED"
        print("PASS: Sub ID replacement")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Sub ID replacement - {e}")
        tc.failed += 1


async def test_max_subscriptions():
    try:
        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            for i in range(8):
                await ws.send(json.dumps(["REQ", f"sub{i}", {"kinds": [99990 + i]}]))
                r = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
                assert r[0] == "EOSE"

            await ws.send(json.dumps(["REQ", "sub8", {"kinds": [99999]}]))
            r = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            assert r[0] == "CLOSED"

            for i in range(8):
                await ws.send(json.dumps(["CLOSE", f"sub{i}"]))
                await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT)
        print("PASS: Max subscriptions (8)")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Max subscriptions - {e}")
        tc.failed += 1


async def main():
    print(f"=== Connection Tests ===\nRelay: {tc.RELAY_URL}\n")

    await test_broadcaster_fanout()
    await asyncio.sleep(3.0)
    await test_multiple_subscribers()
    await asyncio.sleep(3.0)
    await test_ephemeral_events()
    await asyncio.sleep(3.0)
    await test_stored_events()
    await asyncio.sleep(5.0)
    await test_multiple_connections()
    await asyncio.sleep(3.0)
    await test_sub_id_replacement()
    await asyncio.sleep(3.0)
    await test_max_subscriptions()

    return tc.print_results()


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
