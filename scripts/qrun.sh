#!/usr/bin/env bash
# qrun.sh: run a target executable via the qconn launcher and print its stdout/stderr.
# Usage: qrun.sh <abs-path-to-exe> [args...]
# argv0 is set to the exe path automatically.
IP="${QNX_IP:-192.168.1.10}"
PORT="${QNX_PORT:-8000}"
EXE="$1"; shift
ARGS="$*"
{
  printf 'service launcher\n'
  sleep 0.5
  printf 'start/flags 0 %s %s %s\n' "$EXE" "$EXE" "$ARGS"
  sleep 3
} | timeout 12 nc "$IP" "$PORT" 2>&1 | tr -d '\000'
