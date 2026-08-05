#!/usr/bin/env bash
# Copy to env.local.sh and set these to your own addresses. env.local.sh is
# gitignored; this example is not.
#
#   cp scripts/env.example.sh scripts/env.local.sh && $EDITOR scripts/env.local.sh
#
# Every script in scripts/ sources env.local.sh when it exists, so the values
# below are the only place a machine-specific address needs to live. The
# committed defaults are placeholders and are not anyone's real network.

# The CM4 running QNX, reachable on qconn.
export QNX_IP=192.168.1.10
export QNX_PORT=8000

# This machine, serving the staging files over HTTP to the board.
export LAPTOP_IP=192.168.1.11
