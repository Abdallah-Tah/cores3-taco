"""SQLite-backed persistence for capabilities, sensor events, and the event journal."""

from __future__ import annotations

import json
import os
import sqlite3
from datetime import UTC, datetime
from typing import Any

DB_PATH = os.getenv("TACO_DB_PATH", "taco.db")

_SCHEMA = """
CREATE TABLE IF NOT EXISTS capabilities (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS sensor_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    ts TEXT NOT NULL,
    steps_total INTEGER,
    steps_delta INTEGER,
    orientation TEXT,
    pocketed INTEGER,
    raw_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_sensor_events_device_ts
    ON sensor_events (device_id, ts);

CREATE TABLE IF NOT EXISTS journal (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts TEXT NOT NULL,
    device_id TEXT NOT NULL,
    type TEXT NOT NULL,
    payload_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_journal_device_ts
    ON journal (device_id, ts);
"""


def _connect() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    conn.row_factory = sqlite3.Row
    return conn


def utc_now() -> str:
    return datetime.now(UTC).isoformat()


def init_db(defaults: dict[str, Any]) -> None:
    """Create tables if needed and seed any missing capability defaults."""
    conn = _connect()
    try:
        conn.executescript(_SCHEMA)
        existing = {row["key"] for row in conn.execute("SELECT key FROM capabilities")}
        now = utc_now()
        for key, value in defaults.items():
            if key in existing:
                continue
            conn.execute(
                "INSERT INTO capabilities (key, value, updated_at) VALUES (?, ?, ?)",
                (key, json.dumps(value), now),
            )
        conn.commit()
    finally:
        conn.close()


def get_capabilities() -> dict[str, Any]:
    conn = _connect()
    try:
        rows = conn.execute("SELECT key, value FROM capabilities").fetchall()
        return {row["key"]: json.loads(row["value"]) for row in rows}
    finally:
        conn.close()


def set_capabilities(updates: dict[str, Any]) -> dict[str, Any]:
    conn = _connect()
    try:
        now = utc_now()
        for key, value in updates.items():
            conn.execute(
                """
                INSERT INTO capabilities (key, value, updated_at) VALUES (?, ?, ?)
                ON CONFLICT(key) DO UPDATE SET value = excluded.value,
                    updated_at = excluded.updated_at
                """,
                (key, json.dumps(value), now),
            )
        conn.commit()
        rows = conn.execute("SELECT key, value FROM capabilities").fetchall()
        return {row["key"]: json.loads(row["value"]) for row in rows}
    finally:
        conn.close()


def record_sensor_event(
    device_id: str,
    steps_total: int | None,
    steps_delta: int | None,
    orientation: str | None,
    pocketed: bool | None,
    raw: dict[str, Any],
) -> None:
    conn = _connect()
    try:
        conn.execute(
            """
            INSERT INTO sensor_events
                (device_id, ts, steps_total, steps_delta, orientation, pocketed, raw_json)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            """,
            (
                device_id,
                utc_now(),
                steps_total,
                steps_delta,
                orientation,
                None if pocketed is None else int(pocketed),
                json.dumps(raw),
            ),
        )
        conn.commit()
    finally:
        conn.close()


def add_journal_entry(device_id: str, entry_type: str, payload: dict[str, Any]) -> None:
    conn = _connect()
    try:
        conn.execute(
            "INSERT INTO journal (ts, device_id, type, payload_json) VALUES (?, ?, ?, ?)",
            (utc_now(), device_id, entry_type, json.dumps(payload)),
        )
        conn.commit()
    finally:
        conn.close()


def list_journal(limit: int = 100, device_id: str | None = None) -> list[dict[str, Any]]:
    conn = _connect()
    try:
        if device_id:
            rows = conn.execute(
                "SELECT ts, device_id, type, payload_json FROM journal "
                "WHERE device_id = ? ORDER BY id DESC LIMIT ?",
                (device_id, limit),
            ).fetchall()
        else:
            rows = conn.execute(
                "SELECT ts, device_id, type, payload_json FROM journal "
                "ORDER BY id DESC LIMIT ?",
                (limit,),
            ).fetchall()
        return [
            {
                "ts": row["ts"],
                "device_id": row["device_id"],
                "type": row["type"],
                "payload": json.loads(row["payload_json"]),
            }
            for row in rows
        ]
    finally:
        conn.close()
