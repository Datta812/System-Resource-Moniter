#!/usr/bin/env bash
# Build and start both parts. Run from the project folder:  ./run.sh
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O2 -Wall src/main.cpp -o resource_monitor -lsqlite3
./resource_monitor > /dev/null &
MON=$!
trap 'kill -INT $MON 2>/dev/null' EXIT
echo "Monitor running (pid $MON). Dashboard: http://127.0.0.1:5000"
python3 dashboard/app.py
