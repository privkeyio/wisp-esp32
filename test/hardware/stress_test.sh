#!/bin/bash
set -e

RELAY="${RELAY:-ws://192.168.1.100:4869}"
NSEC="${NSEC:-}"
SERIAL="${SERIAL:-/dev/ttyUSB0}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

check_deps() {
    local missing=""
    for cmd in nak websocat timeout bc; do
        command -v $cmd &> /dev/null || missing="$missing $cmd"
    done
    [ -z "$missing" ] && return
    echo "Missing dependencies:$missing"
    exit 1
}

get_heap_size() {
    if [ -c "$SERIAL" ]; then
        timeout 2 cat "$SERIAL" 2>/dev/null | grep -oP 'Free heap: \K\d+' | head -1
    fi
}

log_memory() {
    local heap
    heap=$(get_heap_size)
    if [ -n "$heap" ]; then
        echo -e "${CYAN}[MEM]${NC} Free heap: $heap bytes"
    fi
}

stress_connection_storm() {
    local count=${1:-20}
    local duration=${2:-5}

    echo -e "\n${YELLOW}=== Connection Storm Test ===${NC}"
    echo "Creating $count connections over $duration seconds..."

    log_memory

    local pids=()
    local start_time=$(date +%s)

    for i in $(seq 1 $count); do
        {
            timeout 10 websocat -t "$RELAY" <<< "[\"REQ\",\"storm_$i\",{\"kinds\":[1],\"limit\":1}]" &>/dev/null || true
        } &
        pids+=($!)

        local elapsed=$(($(date +%s) - start_time))
        if [ $elapsed -lt $duration ]; then
            sleep $(echo "scale=3; $duration / $count" | bc)
        fi
    done

    echo "Waiting for connections to complete..."
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done

    log_memory
    echo -e "${GREEN}Connection storm complete${NC}"
}

stress_event_flood() {
    local count=${1:-100}
    local duration=${2:-10}

    echo -e "\n${YELLOW}=== Event Flood Test ===${NC}"

    if [ -z "$NSEC" ]; then
        echo -e "${RED}SKIP${NC}: NSEC not set"
        return
    fi

    echo "Publishing $count events over $duration seconds..."
    log_memory

    local start_time=$(date +%s.%N)
    local success=0
    local failed=0

    for i in $(seq 1 $count); do
        local response
        response=$(nak event --sec "$NSEC" -c "Stress test event $i at $(date +%s%N)" "$RELAY" 2>&1 || echo "error")

        if echo "$response" | grep -q '"id"'; then
            ((success++))
        else
            ((failed++))
        fi

        if [ $((i % 20)) -eq 0 ]; then
            echo "  Published: $i / $count (success: $success, failed: $failed)"
        fi
    done

    local end_time=$(date +%s.%N)
    local elapsed=$(echo "$end_time - $start_time" | bc)
    local rate=$(echo "scale=2; $success / $elapsed" | bc)

    echo ""
    echo "Results:"
    echo "  - Successful: $success"
    echo "  - Failed: $failed"
    echo "  - Duration: ${elapsed}s"
    echo -e "  - Rate: ${CYAN}$rate events/sec${NC}"
    log_memory

    if (( $(echo "$rate >= 10" | bc -l) )); then
        echo -e "${GREEN}PASS${NC}: Achieved >= 10 events/sec"
    else
        echo -e "${RED}WARN${NC}: Below 10 events/sec target"
    fi
}

stress_query_load() {
    local count=${1:-50}

    echo -e "\n${YELLOW}=== Query Load Test ===${NC}"
    echo "Executing $count concurrent queries..."

    log_memory

    local pids=()
    local start_time=$(date +%s.%N)

    for i in $(seq 1 $count); do
        {
            nak req -k 1 --limit 10 "$RELAY" &>/dev/null || true
        } &
        pids+=($!)
    done

    echo "Waiting for queries to complete..."
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done

    local end_time=$(date +%s.%N)
    local elapsed=$(echo "$end_time - $start_time" | bc)

    echo "  Duration: ${elapsed}s"
    log_memory
    echo -e "${GREEN}Query load complete${NC}"
}

stress_subscription_churn() {
    local count=${1:-40}

    echo -e "\n${YELLOW}=== Subscription Churn Test ===${NC}"
    echo "Creating and closing $count subscriptions..."

    log_memory

    for i in $(seq 1 $count); do
        timeout 2 websocat -t "$RELAY" << EOF &>/dev/null || true
["REQ","churn_$i",{"kinds":[1],"limit":1}]
["CLOSE","churn_$i"]
EOF
        sleep 0.1
        if [ $((i % 25)) -eq 0 ]; then
            echo "  Processed: $i / $count"
            sleep 1
        fi
    done

    log_memory
    echo -e "${GREEN}Subscription churn complete${NC}"
}

stress_mixed_load() {
    local duration=${1:-30}

    echo -e "\n${YELLOW}=== Mixed Load Test ===${NC}"
    echo "Running mixed workload for $duration seconds..."

    if [ -z "$NSEC" ]; then
        echo -e "${RED}SKIP${NC}: NSEC not set"
        return
    fi

    log_memory

    local end_time=$(($(date +%s) + duration))
    local events=0
    local queries=0
    local subs=0

    while [ $(date +%s) -lt $end_time ]; do
        nak event --sec "$NSEC" -c "Mixed load $(date +%s%N)" "$RELAY" &>/dev/null &
        ((events++))

        nak req -k 1 --limit 5 "$RELAY" &>/dev/null &
        ((queries++))

        timeout 1 websocat -t "$RELAY" <<< '["REQ","mix",{"kinds":[1],"limit":1}]' &>/dev/null &
        ((subs++))

        sleep 0.2
    done

    wait 2>/dev/null || true

    echo "Results:"
    echo "  - Events published: $events"
    echo "  - Queries executed: $queries"
    echo "  - Subscriptions: $subs"
    log_memory
    echo -e "${GREEN}Mixed load complete${NC}"
}

memory_soak() {
    local hours=${1:-1}
    local interval=${2:-60}

    echo -e "\n${YELLOW}=== Memory Soak Test ===${NC}"
    echo "Running for $hours hour(s), logging every $interval seconds..."

    if [ ! -c "$SERIAL" ]; then
        echo -e "${RED}SKIP${NC}: Serial port $SERIAL not available"
        return
    fi

    local end_time=$(($(date +%s) + hours * 3600))
    local samples=()

    while [ $(date +%s) -lt $end_time ]; do
        local heap
        heap=$(get_heap_size)
        if [ -n "$heap" ]; then
            samples+=("$heap")
            echo "[$(date '+%H:%M:%S')] Heap: $heap bytes"
        fi

        if [ -n "$NSEC" ]; then
            nak event --sec "$NSEC" -c "Soak test $(date +%s)" "$RELAY" &>/dev/null || true
        fi
        nak req -k 1 --limit 5 "$RELAY" &>/dev/null || true

        sleep "$interval"
    done

    if [ ${#samples[@]} -ge 2 ]; then
        local first=${samples[0]}
        local last=${samples[-1]}
        local diff=$((first - last))
        echo ""
        echo "Memory summary:"
        echo "  - Start: $first bytes"
        echo "  - End: $last bytes"
        echo "  - Change: $diff bytes (positive = memory freed)"

        if [ $diff -lt -50000 ]; then
            echo -e "${RED}WARN${NC}: Potential memory leak detected"
        else
            echo -e "${GREEN}PASS${NC}: Memory stable"
        fi
    fi
}

show_usage() {
    echo "Usage: $0 [options] [test...]"
    echo ""
    echo "Options:"
    echo "  -r, --relay URL    Relay WebSocket URL (default: ws://192.168.1.100:4869)"
    echo "  -k, --key NSEC     Nostr secret key for event publishing"
    echo "  -s, --serial PORT  Serial port for memory monitoring"
    echo "  -h, --help         Show this help"
    echo ""
    echo "Tests:"
    echo "  all          Run all stress tests"
    echo "  connections  Connection storm (20 conn/5s)"
    echo "  events       Event flood (100 events/10s)"
    echo "  queries      Query load (50 concurrent)"
    echo "  churn        Subscription churn (40 cycles)"
    echo "  mixed        Mixed workload (30s)"
    echo "  soak         Memory soak (1 hour)"
    echo ""
    echo "Examples:"
    echo "  $0 --relay ws://192.168.1.50:4869 --key nsec1... all"
    echo "  $0 connections events"
    echo "  RELAY=ws://relay:4869 NSEC=nsec1... $0 all"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -r|--relay)
            RELAY="$2"
            shift 2
            ;;
        -k|--key)
            NSEC="$2"
            shift 2
            ;;
        -s|--serial)
            SERIAL="$2"
            shift 2
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            break
            ;;
    esac
done

TESTS="${@:-all}"

echo "=== Wisp ESP32 Stress Tests ==="
echo "Relay: $RELAY"
echo "NSEC: ${NSEC:+set}${NSEC:-not set}"
echo "Serial: ${SERIAL}"
echo ""

check_deps

for test in $TESTS; do
    case $test in
        all)
            stress_connection_storm 20 5
            stress_event_flood 100 10
            stress_query_load 50
            stress_subscription_churn
            stress_mixed_load 30
            ;;
        connections)
            stress_connection_storm 20 5
            ;;
        events)
            stress_event_flood 100 10
            ;;
        queries)
            stress_query_load 50
            ;;
        churn)
            stress_subscription_churn
            ;;
        mixed)
            stress_mixed_load 30
            ;;
        soak)
            memory_soak 1 60
            ;;
        *)
            echo "Unknown test: $test"
            show_usage
            exit 1
            ;;
    esac
done

echo -e "\n${GREEN}=== Stress Tests Complete ===${NC}"
