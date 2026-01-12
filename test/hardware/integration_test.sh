#!/bin/bash
set -e

RELAY="${RELAY:-ws://192.168.1.100:4869}"
HTTP_URL="${HTTP_URL:-http://192.168.1.100:4869}"
NSEC="${NSEC:-}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0

pass() {
    echo -e "${GREEN}PASS${NC}"
    ((PASS++))
}

fail() {
    echo -e "${RED}FAIL${NC}: $1"
    ((FAIL++))
}

skip() {
    echo -e "${YELLOW}SKIP${NC}: $1"
}

check_deps() {
    local missing=""
    for cmd in nak websocat curl jq timeout; do
        command -v $cmd &> /dev/null || missing="$missing $cmd"
    done
    [ -z "$missing" ] && return
    echo "Missing dependencies:$missing"
    echo "Install with: cargo install nak websocat"
    echo "Also need: curl jq (from package manager)"
    exit 1
}

test_connectivity() {
    echo -n "WebSocket connectivity... "
    if timeout 5 websocat -t "$RELAY" <<< '["REQ","test",{}]' &>/dev/null; then
        pass
    else
        fail "Cannot connect to $RELAY"
        exit 1
    fi
}

test_nip11_info() {
    echo -n "NIP-11 relay info... "
    local response
    response=$(curl -s -H "Accept: application/nostr+json" "$HTTP_URL/")
    if echo "$response" | jq -e '.supported_nips' >/dev/null 2>&1; then
        pass
        echo "  NIPs: $(echo "$response" | jq -c '.supported_nips')"
    else
        fail "Invalid NIP-11 response"
    fi
}

test_event_roundtrip() {
    echo -n "Event roundtrip... "
    if [ -z "$NSEC" ]; then
        skip "NSEC not set"
        return
    fi

    local content="Integration test $(date +%s)"
    local id
    id=$(nak event --sec "$NSEC" -c "$content" "$RELAY" 2>/dev/null | jq -r '.id' 2>/dev/null)

    if [ -z "$id" ] || [ "$id" = "null" ]; then
        fail "Failed to publish event"
        return
    fi

    sleep 0.5
    local result
    result=$(nak req --id "$id" "$RELAY" 2>/dev/null | head -1 | jq -r '.[2].id' 2>/dev/null)

    if [ "$id" = "$result" ]; then
        pass
    else
        fail "Event not found after publish (id=$id)"
    fi
}

test_subscription_delivery() {
    echo -n "Subscription delivery... "
    if [ -z "$NSEC" ]; then
        skip "NSEC not set"
        return
    fi

    local marker="subtest_$(date +%s%N)"
    local received=""

    {
        sleep 1
        nak event --sec "$NSEC" -c "$marker" "$RELAY" &>/dev/null
    } &

    received=$(timeout 5 nak req -k 1 --limit 0 "$RELAY" 2>/dev/null | grep -m1 "$marker" || true)

    if [ -n "$received" ]; then
        pass
    else
        fail "Subscription did not receive published event"
    fi
}

test_eose_response() {
    echo -n "EOSE after historical... "
    local response
    response=$(timeout 3 websocat -t "$RELAY" <<< '["REQ","eose_test",{"kinds":[1],"limit":1}]' 2>/dev/null | head -5)

    if echo "$response" | grep -q '"EOSE"'; then
        pass
    else
        fail "No EOSE received"
    fi
}

test_close_subscription() {
    echo -n "Close subscription... "
    local response
    response=$(timeout 3 websocat -t "$RELAY" << 'EOF' 2>/dev/null
["REQ","close_test",{"kinds":[1],"limit":1}]
["CLOSE","close_test"]
EOF
)
    if echo "$response" | grep -q '"CLOSED"'; then
        pass
    else
        fail "No CLOSED response"
    fi
}

test_invalid_json_rejected() {
    echo -n "Invalid JSON rejected... "
    local response
    response=$(timeout 3 websocat -t "$RELAY" <<< 'not valid json' 2>/dev/null | head -1)

    if echo "$response" | grep -qi 'notice\|error'; then
        pass
    else
        fail "No error notice for invalid JSON"
    fi
}

test_invalid_message_rejected() {
    echo -n "Invalid message format rejected... "
    local response
    response=$(timeout 3 websocat -t "$RELAY" <<< '["INVALID_TYPE"]' 2>/dev/null | head -1)

    if echo "$response" | grep -qi 'notice\|error\|unknown'; then
        pass
    else
        fail "No error for invalid message type"
    fi
}

test_invalid_signature_rejected() {
    echo -n "Invalid signature rejected... "
    local bad_event='["EVENT",{"id":"0000000000000000000000000000000000000000000000000000000000000000","pubkey":"0000000000000000000000000000000000000000000000000000000000000000","created_at":1234567890,"kind":1,"tags":[],"content":"test","sig":"0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"}]'

    local response
    response=$(timeout 3 websocat -t "$RELAY" <<< "$bad_event" 2>/dev/null | head -1)

    if echo "$response" | grep -qi 'false\|invalid\|signature'; then
        pass
    else
        fail "Invalid signature not rejected"
    fi
}

test_rate_limiting() {
    echo -n "Rate limiting... "
    if [ -z "$NSEC" ]; then
        skip "NSEC not set"
        return
    fi

    local limited=0
    for i in {1..35}; do
        local response
        response=$(nak event --sec "$NSEC" -c "Rate test $i" "$RELAY" 2>&1 || true)
        if echo "$response" | grep -qi "rate\|limit\|blocked\|false"; then
            limited=1
            break
        fi
    done

    if [ $limited -eq 1 ]; then
        pass
    else
        fail "No rate limiting after 35 rapid events"
    fi
}

test_multiple_subscriptions() {
    echo -n "Multiple subscriptions... "
    local response
    response=$(timeout 3 websocat -t "$RELAY" << 'EOF' 2>/dev/null
["REQ","sub1",{"kinds":[1],"limit":1}]
["REQ","sub2",{"kinds":[0],"limit":1}]
["REQ","sub3",{"kinds":[3],"limit":1}]
EOF
)
    local eose_count
    eose_count=$(echo "$response" | grep -c '"EOSE"' || echo 0)

    if [ "$eose_count" -ge 3 ]; then
        pass
    else
        fail "Expected 3 EOSE responses, got $eose_count"
    fi
}

echo "=== Wisp ESP32 Integration Tests ==="
echo "Relay: $RELAY"
echo ""

check_deps
test_connectivity
test_nip11_info
test_event_roundtrip
test_subscription_delivery
test_eose_response
test_close_subscription
test_invalid_json_rejected
test_invalid_message_rejected
test_invalid_signature_rejected
test_rate_limiting
test_multiple_subscriptions

echo ""
echo "=== Results ==="
echo -e "Passed: ${GREEN}$PASS${NC}"
echo -e "Failed: ${RED}$FAIL${NC}"

if [ $FAIL -gt 0 ]; then
    exit 1
fi
