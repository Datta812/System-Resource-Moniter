# OS System Resource Monitor

A small Linux system monitor built for the **CSE 302 – Operating Systems** course (SRM University, AP).
It measures **CPU, memory, disk I/O and network bandwidth** every second, **stores the history in a SQLite database**, and shows **live values and utilization graphs** on a web dashboard.

Tools like `top` show only the current moment. This project keeps history, so you can look back and see what caused a spike.

## Features

- Live cards for CPU, memory (used / total), disk read and write, network download and upload
- Load average (1 / 5 / 15 min), running and total process counts, and system uptime
- Six history graphs (Chart.js) with a selectable window: 1, 5, 15 or 60 minutes
- Every sample saved to SQLite (one row per second), about 24 hours kept automatically
- REST API with JSON output, plus a CSV download
- Dashboard shows "Monitor stopped" if the collector is not running

## How it works

```
Linux /proc files  ->  C++ collector  ->  SQLite database  ->  Flask REST API  ->  Browser dashboard
 (kernel data)         (1 sample/sec)     data/resources.db     (read-only)        (updates every 2 s)
```

The collector and the web server are two separate programs. The collector writes to the database, and Flask only reads from it.

| Metric | Kernel file | How it is calculated |
|---|---|---|
| CPU % | `/proc/stat` | `100 x (delta total - delta idle) / delta total` between two samples (idle includes I/O wait) |
| Memory % | `/proc/meminfo` | `(MemTotal - MemAvailable) / MemTotal`; available memory is `MemAvailable` |
| Disk read / write KB/s | `/proc/diskstats` | change in 512-byte sectors x 0.5 / elapsed seconds (whole disks only, partitions skipped) |
| Network KB/s | `/proc/net/dev` | change in bytes / 1024 / elapsed seconds (loopback skipped) |
| Load average, processes | `/proc/loadavg` | read directly (the process total counts kernel threads) |
| Uptime | `/proc/uptime` | read directly |

The kernel files hold running totals since boot, so every rate is calculated from the **difference between two consecutive samples**, divided by the exact time between them.

## Project structure

```
.
├── src/
│   └── main.cpp                  # C++ collector: reads /proc, calculates metrics, writes to SQLite
├── dashboard/
│   ├── app.py                    # Flask server and REST API
│   ├── templates/index.html      # Dashboard page and Chart.js code
│   └── static/style.css          # Dashboard styling
├── data/                         # resources.db is created here at runtime
├── docs/screenshots/             # Screenshots for this README
├── run.sh                        # Builds and starts the collector and the dashboard
├── requirements.txt              # Python dependency (Flask)
└── README.md
```

## Requirements

- Linux (Ubuntu recommended). On Windows, use **WSL 2**
- `g++` with C++17 support
- SQLite development library
- Python 3 and Flask
- Internet access in the browser on first load (Chart.js is loaded from a CDN)

Install on Ubuntu / Debian:

```bash
sudo apt update
sudo apt install g++ libsqlite3-dev python3 python3-venv sqlite3
```

## Setup and run

```bash
git clone https://github.com/<your-username>/<your-repo>.git
cd <your-repo>

python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

chmod +x run.sh
./run.sh
```

Then open **http://127.0.0.1:5000** in your browser. Press `Ctrl+C` in the terminal to stop both the collector and the dashboard.

To run the two parts manually, in two terminals:

```bash
g++ -std=c++17 -O2 -Wall src/main.cpp -o resource_monitor -lsqlite3
./resource_monitor            # terminal 1: collector

source .venv/bin/activate
python3 dashboard/app.py      # terminal 2: dashboard
```

Run both from the project folder so that `data/resources.db` is found.

> If `./run.sh` says "No such file or directory", the file has Windows line endings. Fix with `sed -i 's/\r$//' run.sh`.

## REST API

| Endpoint | Description |
|---|---|
| `GET /` | Dashboard page |
| `GET /api/latest` | Newest measurement as JSON (with `age_seconds`) |
| `GET /api/history?limit=N` | Last N measurements, oldest first (1 to 3600, default 60) |
| `GET /api/summary?limit=N` | Average and peak values over the last N samples |
| `GET /api/export.csv?limit=N` | History as a CSV download |
| `GET /api/health` | Service status |

## Database

One table, `resource_history`, with `id` as the primary key and one row per second:

`id, timestamp, cpu_percent, memory_total_kb, memory_used_kb, memory_percent, disk_read_kbps, disk_write_kbps, network_download_kbps, network_upload_kbps, load_1, load_5, load_15, uptime_seconds, running_processes, total_processes`

- Timestamps are stored in **UTC** (IST is UTC + 5:30)
- WAL mode lets the dashboard read while the collector writes
- Rows older than the latest 86,400 (about 24 hours) are deleted automatically

Inspect it:

```bash
sqlite3 data/resources.db "select * from resource_history order by id desc limit 5;"
```

## Try it: create load and watch the dashboard react

Compare the dashboard with `top` and `free -m` in another terminal.

```bash
# CPU: each command keeps one logical CPU busy (run 4 times, about 25% on 16 CPUs)
yes > /dev/null &
killall yes                                   # stop it

# Memory: holds 2 GB until you press Enter
python3 -c "x = bytearray(2*1024**3); input()"

# Disk write and read (writes to your home folder, bypassing the cache)
dd if=/dev/zero of=~/test.bin bs=1M count=1000 oflag=direct conv=fsync
dd if=~/test.bin of=/dev/null bs=1M iflag=direct
rm ~/test.bin

# Network: downloads 10 MB and discards it
curl -L -o /dev/null "https://proof.ovh.net/files/10Mb.dat"

# Processes: starts 100 sleeping processes
set +m; for i in $(seq 1 100); do sleep 300 & done
killall sleep
```

## Notes and limitations

- Linux only. Under **WSL 2**, `/proc` describes the Linux virtual machine, so the values will **not** match Windows Task Manager
- Shows total CPU, not per-core or per-process usage
- Swap and page cache are not shown separately
- No authentication; the server listens on all interfaces, so use it on a trusted network only

## Possible future work

Per-core CPU usage, a top-processes table, swap reporting, alert thresholds, and authentication.

## Team

| Name | Registration No. |
|---|---|
| Siddireddy Manoj Reddy | AP24110010469 |
| Gundapaneni Kusadatta | AP24110011788 |
| Mandadapu Naga Manikanta | AP24110011800 |
| Pillimitla Yeswanth | AP24110011159 |

Course: CSE 302 – Operating Systems, Department of Computer Science and Engineering, SRM University, AP. Faculty mentor: Dr. Mallikarjuna.
