import csv
import io
import sqlite3
from pathlib import Path

from flask import Flask, Response, jsonify, render_template, request

app = Flask(__name__)

# Project layout:  project/data/resources.db  and  project/<this folder>/app.py
PROJECT_DIR = Path(__file__).resolve().parent.parent
DB_PATH = PROJECT_DIR / "data" / "resources.db"

MAX_HISTORY = 3600  # never return more than one hour of 1-second samples


def get_db_connection():
    # Read-only connection: the dashboard must never modify the monitor's data.
    connection = sqlite3.connect(f"file:{DB_PATH}?mode=ro", uri=True, timeout=3)
    connection.row_factory = sqlite3.Row
    return connection


def query(sql, params=()):
    """Run a SELECT and return rows, or None if the database is not ready yet."""
    if not DB_PATH.exists():
        return None
    connection = get_db_connection()
    try:
        return connection.execute(sql, params).fetchall()
    except sqlite3.OperationalError:
        return None
    finally:
        connection.close()


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/latest")
def latest():
    rows = query("SELECT * FROM resource_history ORDER BY id DESC LIMIT 1")
    if not rows:
        return jsonify({"error": "No measurements recorded yet"}), 404

    data = dict(rows[0])
    # Age of the newest sample (seconds) lets the page detect a stopped monitor.
    age = query(
        "SELECT CAST(strftime('%s','now') AS INTEGER) - "
        "CAST(strftime('%s', timestamp) AS INTEGER) AS age "
        "FROM resource_history WHERE id = ?", (data["id"],))
    data["age_seconds"] = age[0]["age"] if age else None
    return jsonify(data)


@app.route("/api/history")
def history():
    limit = request.args.get("limit", default=60, type=int)
    limit = max(1, min(limit, MAX_HISTORY))

    rows = query(
        "SELECT * FROM (SELECT * FROM resource_history "
        "ORDER BY id DESC LIMIT ?) ORDER BY id ASC", (limit,))
    if rows is None:
        return jsonify([])
    return jsonify([dict(row) for row in rows])


@app.route("/api/summary")
def summary():
    """Average / peak values over the most recent N samples."""
    limit = request.args.get("limit", default=60, type=int)
    limit = max(1, min(limit, MAX_HISTORY))

    rows = query("""
        SELECT COUNT(*) AS samples,
               AVG(cpu_percent) AS cpu_avg, MAX(cpu_percent) AS cpu_max,
               AVG(memory_percent) AS memory_avg, MAX(memory_percent) AS memory_max,
               MAX(disk_read_kbps) AS disk_read_max,
               MAX(disk_write_kbps) AS disk_write_max,
               MAX(network_download_kbps) AS download_max,
               MAX(network_upload_kbps) AS upload_max
        FROM (SELECT * FROM resource_history ORDER BY id DESC LIMIT ?)
    """, (limit,))
    if not rows or rows[0]["samples"] == 0:
        return jsonify({"error": "No measurements recorded yet"}), 404
    return jsonify(dict(rows[0]))


@app.route("/api/export.csv")
def export_csv():
    limit = request.args.get("limit", default=MAX_HISTORY, type=int)
    limit = max(1, min(limit, 86400))
    rows = query(
        "SELECT * FROM (SELECT * FROM resource_history "
        "ORDER BY id DESC LIMIT ?) ORDER BY id ASC", (limit,)) or []

    buffer = io.StringIO()
    if rows:
        writer = csv.writer(buffer)
        writer.writerow(rows[0].keys())
        writer.writerows([tuple(row) for row in rows])

    return Response(
        buffer.getvalue(), mimetype="text/csv",
        headers={"Content-Disposition": "attachment; filename=resource_history.csv"})


@app.route("/api/health")
def health():
    return jsonify({"status": "ok", "database_exists": DB_PATH.exists()})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
