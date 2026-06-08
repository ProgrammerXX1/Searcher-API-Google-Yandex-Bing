#!/bin/bash
# Auto-restart wrapper for SearchX — restarts on crash (SEGV from std::regex)
cd "$(dirname "$0")"
PORT="${1:-8085}"
while true; do
    echo "[$(date '+%H:%M:%S')] Starting SearchX on port $PORT..."
    ./cpp/build/searchx --port "$PORT" --env .env --html index.html
    EXIT=$?
    echo "[$(date '+%H:%M:%S')] SearchX exited with code $EXIT, restarting in 1s..."
    sleep 1
done
