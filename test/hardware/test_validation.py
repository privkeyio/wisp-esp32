#!/usr/bin/env python3
"""TTL and rate limiting tests."""

import asyncio
import json
import sys
import time

import test_common as tc

if not tc.websockets:
    print("Install websockets: pip install websockets")
    sys.exit(1)


async def test_reject_future_event():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Reject future event")
        tc.skipped += 1
        return

    try:
        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            future_time = int(time.time()) + tc.FUTURE_THRESHOLD + 1
            event = tc.make_signed_event("future event", kind=1, created_at=future_time)
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is False
            assert "future" in tc.get_ok_error(ok).lower()
            print("PASS: Future event rejected")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: Reject future event - {e}")
        tc.failed += 1


async def test_reject_expired_nip40():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Reject expired NIP-40")
        tc.skipped += 1
        return

    try:
        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            past_time = int(time.time()) - tc.ONE_HOUR
            event = tc.make_signed_event("expired", kind=1, tags=[["expiration", str(past_time)]])
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is False
            assert "expir" in tc.get_ok_error(ok).lower()
            print("PASS: Expired NIP-40 rejected")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: Reject expired NIP-40 - {e}")
        tc.failed += 1


async def test_accept_valid_nip40():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Accept valid NIP-40")
        tc.skipped += 1
        return

    try:
        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            future_time = int(time.time()) + tc.ONE_HOUR
            event = tc.make_signed_event("valid", kind=1, tags=[["expiration", str(future_time)]])
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is True
            print("PASS: Valid NIP-40 accepted")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: Accept valid NIP-40 - {e}")
        tc.failed += 1


async def test_reject_old_event():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Reject old event")
        tc.skipped += 1
        return

    try:
        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            old_time = int(time.time()) - tc.MAX_EVENT_AGE - 1
            event = tc.make_signed_event("old", kind=1, created_at=old_time)
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is False
            print("PASS: Old event rejected")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: Reject old event - {e}")
        tc.failed += 1


async def test_rate_limit_events():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Rate limit events")
        tc.skipped += 1
        return

    ws = None
    try:
        ws = await tc.websockets.connect(tc.RELAY_URL)
        rate_limited = False
        for i in range(tc.RATE_LIMIT_EVENTS + 1):
            event = tc.make_signed_event(f"rate {i}", kind=1)
            await ws.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            if ok[2] is False and "rate" in tc.get_ok_error(ok).lower():
                rate_limited = True
                break
            await asyncio.sleep(0.15)
        assert rate_limited
        print(f"PASS: Event rate limiting ({tc.RATE_LIMIT_EVENTS}/min)")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Rate limit events - {e}")
        tc.failed += 1
    finally:
        await tc.cleanup_websockets(ws, delay=3.0)


async def test_rate_limit_reqs():
    ws = None
    try:
        ws = await tc.websockets.connect(tc.RELAY_URL)
        rate_limited = False
        for i in range(tc.RATE_LIMIT_REQS + 5):
            await ws.send(json.dumps(["REQ", f"sub{i}", {"kinds": [99999]}]))
            resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            reason = resp[2] if len(resp) > 2 else ""
            if resp[0] == "CLOSED" and "rate" in reason.lower():
                rate_limited = True
                break
            await asyncio.sleep(0.1)
        assert rate_limited
        print(f"PASS: REQ rate limiting ({tc.RATE_LIMIT_REQS}/min)")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Rate limit REQs - {e}")
        tc.failed += 1
    finally:
        await tc.cleanup_websockets(ws, delay=3.0)


async def test_rate_limit_reset():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Rate limit reset")
        tc.skipped += 1
        return

    ws1 = None
    ws2 = None
    try:
        ws1 = await tc.websockets.connect(tc.RELAY_URL)
        rate_limited = False
        for i in range(tc.RATE_LIMIT_EVENTS + 1):
            event = tc.make_signed_event(f"reset {i}", kind=1)
            await ws1.send(json.dumps(["EVENT", event]))
            ok = json.loads(await asyncio.wait_for(ws1.recv(), timeout=tc.TIMEOUT))
            if ok[2] is False and "rate" in tc.get_ok_error(ok).lower():
                rate_limited = True
                break
            await asyncio.sleep(0.15)
        assert rate_limited, "Rate limiting never triggered"
        await tc.close_ws(ws1)
        ws1 = None
        await asyncio.sleep(3.0)

        ws2 = await tc.websockets.connect(tc.RELAY_URL)
        event = tc.make_signed_event("after reconnect", kind=1)
        await ws2.send(json.dumps(["EVENT", event]))
        ok = json.loads(await asyncio.wait_for(ws2.recv(), timeout=tc.TIMEOUT))
        assert ok[2] is True
        print("PASS: Rate limit resets on disconnect")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Rate limit reset - {e}")
        tc.failed += 1
    finally:
        await tc.cleanup_websockets(ws1, ws2)


async def main():
    print(f"=== Validation Tests ===\nRelay: {tc.RELAY_URL}\n")

    await test_reject_future_event()
    await test_reject_expired_nip40()
    await test_accept_valid_nip40()
    await test_reject_old_event()
    await asyncio.sleep(3.0)
    await test_rate_limit_events()
    await asyncio.sleep(5.0)
    await test_rate_limit_reqs()
    await asyncio.sleep(5.0)
    await test_rate_limit_reset()

    return tc.print_results()


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
