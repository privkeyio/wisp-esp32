#!/usr/bin/env python3
"""Protocol parsing and NIP-11 tests."""

import asyncio
import json
import sys
import urllib.request

import test_common as tc

if not tc.websockets:
    print("Install websockets: pip install websockets")
    sys.exit(1)


async def test_event_parsing():
    try:
        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            event = tc.make_event("test event")
            response = await tc.send_recv(ws, ["EVENT", event])
            data = json.loads(response)
            assert data[0] == "OK"
            assert data[1] == event["id"]
            assert data[2] is False
            print("PASS: EVENT parsing")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: EVENT parsing - {e}")
        tc.failed += 1


async def test_req_parsing():
    try:
        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            response = await tc.send_recv(ws, ["REQ", "testsub", {"kinds": [99999], "limit": 10}])
            data = json.loads(response)
            assert data[0] == "EOSE"
            assert data[1] == "testsub"
            print("PASS: REQ parsing")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: REQ parsing - {e}")
        tc.failed += 1


async def test_close_parsing():
    try:
        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            response = await tc.send_recv(ws, ["CLOSE", "testsub"])
            data = json.loads(response)
            assert data[0] == "CLOSED"
            assert data[1] == "testsub"
            print("PASS: CLOSE parsing")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: CLOSE parsing - {e}")
        tc.failed += 1


async def test_invalid_json():
    try:
        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            await ws.send("not valid json")
            response = await asyncio.wait_for(ws.recv(), timeout=tc.TIMEOUT)
            data = json.loads(response)
            assert data[0] == "NOTICE"
            print("PASS: Invalid JSON")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: Invalid JSON - {e}")
        tc.failed += 1


async def test_unknown_message():
    try:
        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            response = await tc.send_recv(ws, ["UNKNOWN", "data"])
            data = json.loads(response)
            assert data[0] == "NOTICE"
            print("PASS: Unknown message")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: Unknown message - {e}")
        tc.failed += 1


async def test_multiple_filters():
    try:
        async with tc.websockets.connect(tc.RELAY_URL) as ws:
            response = await tc.send_recv(ws, ["REQ", "multi", {"kinds": [99998]}, {"kinds": [99997]}])
            data = json.loads(response)
            assert data[0] == "EOSE"
            assert data[1] == "multi"
            print("PASS: Multiple filters")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: Multiple filters - {e}")
        tc.failed += 1


def test_nip11_json_response():
    http_url = tc.RELAY_URL.replace("ws://", "http://").replace("wss://", "https://")
    try:
        req = urllib.request.Request(http_url)
        with urllib.request.urlopen(req, timeout=5) as resp:
            content_type = resp.headers.get("Content-Type", "")
            data = json.loads(resp.read().decode())
            assert "application/json" in content_type
            assert "name" in data
            assert "supported_nips" in data
            assert "limitation" in data
            print("PASS: NIP-11 JSON response")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: NIP-11 JSON response - {e}")
        tc.failed += 1


def test_nip11_accept_header():
    http_url = tc.RELAY_URL.replace("ws://", "http://").replace("wss://", "https://")
    try:
        req = urllib.request.Request(http_url)
        req.add_header("Accept", "application/nostr+json")
        with urllib.request.urlopen(req, timeout=5) as resp:
            content_type = resp.headers.get("Content-Type", "")
            assert "application/nostr+json" in content_type
            print("PASS: NIP-11 Accept header")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: NIP-11 Accept header - {e}")
        tc.failed += 1


def test_nip11_cors_headers():
    http_url = tc.RELAY_URL.replace("ws://", "http://").replace("wss://", "https://")
    try:
        req = urllib.request.Request(http_url)
        with urllib.request.urlopen(req, timeout=5) as resp:
            assert resp.headers.get("Access-Control-Allow-Origin") == "*"
            assert "Accept" in resp.headers.get("Access-Control-Allow-Headers", "")
            assert "GET" in resp.headers.get("Access-Control-Allow-Methods", "")
            print("PASS: NIP-11 CORS headers")
            tc.passed += 1
    except Exception as e:
        print(f"FAIL: NIP-11 CORS headers - {e}")
        tc.failed += 1


async def main():
    print(f"=== Protocol Tests ===\nRelay: {tc.RELAY_URL}\n")

    await test_event_parsing()
    await test_req_parsing()
    await test_close_parsing()
    await test_invalid_json()
    await test_unknown_message()
    await test_multiple_filters()
    test_nip11_json_response()
    test_nip11_accept_header()
    test_nip11_cors_headers()

    return tc.print_results()


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
