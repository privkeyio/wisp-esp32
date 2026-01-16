#!/usr/bin/env python3
"""NIP-09 event deletion tests."""

import asyncio
import json
import sys

import test_common as tc

if not tc.websockets:
    print("Install websockets: pip install websockets")
    sys.exit(1)


async def test_delete_own_event():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Delete own event")
        tc.skipped += 1
        return

    from pynostr.key import PrivateKey
    from pynostr.event import Event as PynostrEvent

    try:
        privkey = PrivateKey()
        ws = await tc.websockets.connect(tc.RELAY_URL)

        event = PynostrEvent(kind=1, content="delete me")
        event.sign(privkey.hex())
        event_dict = event.to_dict()
        event_id = event_dict["id"]

        await ws.send(json.dumps(["EVENT", event_dict]))
        ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
        assert ok[2] is True

        await ws.send(json.dumps(["REQ", "check1", {"ids": [event_id]}]))
        found = False
        while True:
            msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            if msg[0] == "EVENT":
                found = True
            elif msg[0] == "EOSE":
                break
        assert found

        deletion = PynostrEvent(kind=5, content="test deletion")
        deletion.tags = [["e", event_id], ["k", "1"]]
        deletion.sign(privkey.hex())
        await ws.send(json.dumps(["EVENT", deletion.to_dict()]))
        ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
        assert ok[2] is True

        await ws.send(json.dumps(["CLOSE", "check1"]))
        await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT)

        await ws.send(json.dumps(["REQ", "check2", {"ids": [event_id]}]))
        deleted = True
        while True:
            msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            if msg[0] == "EVENT" and msg[2]["id"] == event_id:
                deleted = False
            elif msg[0] == "EOSE":
                break

        await tc.close_ws(ws)
        if not deleted:
            print(f"FAIL: Delete own event - event still exists after deletion")
            tc.failed += 1
            return
        print("PASS: Delete own event")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Delete own event - {e}")
        tc.failed += 1


async def test_unauthorized_deletion():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Unauthorized deletion")
        tc.skipped += 1
        return

    from pynostr.key import PrivateKey
    from pynostr.event import Event as PynostrEvent

    try:
        author_key = PrivateKey()
        attacker_key = PrivateKey()
        ws = await tc.websockets.connect(tc.RELAY_URL)

        event = PynostrEvent(kind=1, content="protected event")
        event.sign(author_key.hex())
        event_dict = event.to_dict()
        event_id = event_dict["id"]

        await ws.send(json.dumps(["EVENT", event_dict]))
        ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
        assert ok[2] is True

        deletion = PynostrEvent(kind=5, content="unauthorized")
        deletion.tags = [["e", event_id], ["k", "1"]]
        deletion.sign(attacker_key.hex())
        await ws.send(json.dumps(["EVENT", deletion.to_dict()]))
        ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))

        await ws.send(json.dumps(["REQ", "check", {"ids": [event_id]}]))
        still_exists = False
        while True:
            msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            if msg[0] == "EVENT" and msg[2]["id"] == event_id:
                still_exists = True
            elif msg[0] == "EOSE":
                break

        await tc.close_ws(ws)
        assert still_exists
        print("PASS: Unauthorized deletion rejected")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Unauthorized deletion - {e}")
        tc.failed += 1


async def test_delete_nonexistent():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Delete nonexistent")
        tc.skipped += 1
        return

    from pynostr.key import PrivateKey
    from pynostr.event import Event as PynostrEvent

    try:
        privkey = PrivateKey()
        fake_id = "0" * 64

        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            deletion = PynostrEvent(kind=5, content="delete nothing")
            deletion.tags = [["e", fake_id], ["k", "1"]]
            deletion.sign(privkey.hex())
            await ws.send(json.dumps(["EVENT", deletion.to_dict()]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            assert ok[0] == "OK"
            assert ok[2] is True
        print("PASS: Delete nonexistent (silent success)")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Delete nonexistent - {e}")
        tc.failed += 1


async def test_multiple_deletions():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Multiple deletions")
        tc.skipped += 1
        return

    from pynostr.key import PrivateKey
    from pynostr.event import Event as PynostrEvent

    try:
        privkey = PrivateKey()
        ws = await tc.websockets.connect(tc.RELAY_URL)

        event_ids = []
        for i in range(3):
            event = PynostrEvent(kind=1, content=f"multi delete {i}")
            event.sign(privkey.hex())
            event_dict = event.to_dict()
            event_ids.append(event_dict["id"])
            await ws.send(json.dumps(["EVENT", event_dict]))
            ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            assert ok[2] is True

        deletion = PynostrEvent(kind=5, content="batch delete")
        deletion.tags = [["e", eid] for eid in event_ids] + [["k", "1"]]
        deletion.sign(privkey.hex())
        await ws.send(json.dumps(["EVENT", deletion.to_dict()]))
        ok = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
        assert ok[2] is True

        await ws.send(json.dumps(["REQ", "check", {"ids": event_ids}]))
        found_count = 0
        while True:
            msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT))
            if msg[0] == "EVENT":
                found_count += 1
            elif msg[0] == "EOSE":
                break

        await tc.close_ws(ws)
        assert found_count == 0
        print("PASS: Multiple e tags deletion")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Multiple deletions - {e}")
        tc.failed += 1


async def test_deletion_broadcast():
    if not tc.HAS_PYNOSTR:
        print("SKIP: Deletion broadcast")
        tc.skipped += 1
        return

    from pynostr.key import PrivateKey
    from pynostr.event import Event as PynostrEvent

    try:
        privkey = PrivateKey()
        ws1 = await tc.websockets.connect(tc.RELAY_URL)
        await asyncio.sleep(tc.CONNECTION_DELAY)
        ws2 = await tc.websockets.connect(tc.RELAY_URL)

        import time
        await ws1.send(json.dumps(["REQ", "sub", {"kinds": [5], "since": int(time.time()) - 5}]))
        while True:
            msg = json.loads(await asyncio.wait_for(ws1.recv(), timeout=tc.TIMEOUT))
            if msg[0] == "EOSE":
                break

        event = PynostrEvent(kind=1, content="will be deleted")
        event.sign(privkey.hex())
        event_dict = event.to_dict()
        event_id = event_dict["id"]

        await ws2.send(json.dumps(["EVENT", event_dict]))
        ok = json.loads(await asyncio.wait_for(ws2.recv(), timeout=tc.TIMEOUT))
        assert ok[2] is True

        deletion = PynostrEvent(kind=5, content="broadcast test")
        deletion.tags = [["e", event_id], ["k", "1"]]
        deletion.sign(privkey.hex())
        await ws2.send(json.dumps(["EVENT", deletion.to_dict()]))
        ok = json.loads(await asyncio.wait_for(ws2.recv(), timeout=tc.TIMEOUT))
        assert ok[2] is True

        broadcast = json.loads(await asyncio.wait_for(ws1.recv(), timeout=tc.TIMEOUT))
        assert broadcast[0] == "EVENT"
        assert broadcast[2]["kind"] == 5

        await tc.close_ws(ws1)
        await tc.close_ws(ws2)
        print("PASS: Deletion event broadcast")
        tc.passed += 1
    except Exception as e:
        print(f"FAIL: Deletion broadcast - {e}")
        tc.failed += 1


async def main():
    print(f"=== NIP-09 Deletion Tests ===\nRelay: {tc.RELAY_URL}\n")

    await test_delete_own_event()
    await asyncio.sleep(2.0)
    await test_unauthorized_deletion()
    await asyncio.sleep(2.0)
    await test_delete_nonexistent()
    await asyncio.sleep(2.0)
    await test_multiple_deletions()
    await asyncio.sleep(2.0)
    await test_deletion_broadcast()

    return tc.print_results()


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
