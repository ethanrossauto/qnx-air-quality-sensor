#!/usr/bin/env bash
# qsh.sh "<shell command>" [wait_secs]
# Runs `ksh -c "<cmd>"` on the QNX target via the qconn launcher and prints stdout/stderr.
IP="${QNX_IP:-192.168.1.10}"; PORT="${QNX_PORT:-8000}"
CMD="$1"; WAIT="${2:-3}"
{
  printf 'service launcher\n'; sleep 0.5
  printf 'start/flags 0 /proc/boot/ksh ksh -c "%s"\n' "$CMD"
  sleep "$WAIT"
} | timeout $((WAIT+8)) nc "$IP" "$PORT" 2>&1 | tr -d '\000' | sed 's/QCONN//; /^OK [0-9]*$/d; /^OK$/d'
