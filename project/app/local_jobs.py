"""Durable request cache and offline outbox, independent from dashboard history."""

import hashlib, json, sqlite3, time
from pathlib import Path


class LocalJobs:
    def __init__(self, data_dir):
        self.root = Path(data_dir) / "hybrid"
        self.root.mkdir(parents=True, exist_ok=True)
        self.db = self.root / "jobs.sqlite3"
        with self.connect() as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.execute(
                "CREATE TABLE IF NOT EXISTS jobs(id TEXT PRIMARY KEY, device TEXT NOT NULL, owner TEXT NOT NULL, digest TEXT NOT NULL, result TEXT, synced INTEGER DEFAULT 0, displayed INTEGER, played INTEGER, updated REAL NOT NULL)"
            )

    def connect(self):
        db = sqlite3.connect(self.db, timeout=15)
        db.row_factory = sqlite3.Row
        return closing_connection(db)

    def put_image(self, request_id, device, owner, raw):
        digest = hashlib.sha256(raw).hexdigest()
        with self.connect() as db:
            row = db.execute("SELECT * FROM jobs WHERE id=?", (request_id,)).fetchone()
            if row and (
                row["digest"] != digest or row["device"] != device or row["owner"] != owner
            ):
                raise ValueError("request_id_conflict")
            if not row:
                db.execute(
                    "INSERT INTO jobs(id,device,owner,digest,updated) VALUES(?,?,?,?,?)",
                    (request_id, device, owner, digest, time.time()),
                )
        path = self.root / (request_id + ".jpg")
        if not path.exists():
            tmp = path.with_suffix(".tmp")
            tmp.write_bytes(raw)
            tmp.replace(path)
        return self.get(request_id)

    def get(self, request_id):
        with self.connect() as db:
            row = db.execute("SELECT * FROM jobs WHERE id=?", (request_id,)).fetchone()
        if not row:
            return None
        value = dict(row)
        value["result"] = json.loads(value["result"]) if value["result"] else None
        return value

    def complete(self, request_id, result):
        with self.connect() as db:
            db.execute(
                "UPDATE jobs SET result=?,updated=? WHERE id=?",
                (json.dumps(result, ensure_ascii=False), time.time(), request_id),
            )

    def ack(self, request_id, displayed, played):
        with self.connect() as db:
            db.execute(
                "UPDATE jobs SET displayed=?,played=?,synced=0,updated=? WHERE id=?",
                (int(displayed), int(played), time.time(), request_id),
            )

    def pending(self):
        with self.connect() as db:
            ids = [
                r[0]
                for r in db.execute(
                    "SELECT id FROM jobs WHERE result IS NOT NULL AND synced=0 ORDER BY updated LIMIT 10"
                )
            ]
        return [self.get(i) for i in ids]

    def mark_synced(self, request_id, updated):
        with self.connect() as db:
            db.execute("UPDATE jobs SET synced=1 WHERE id=? AND updated=?", (request_id, updated))

    def prune(self):
        """Keep unsynced requests and at most 50 local records once synced."""
        with self.connect() as db:
            ids = [
                r[0]
                for r in db.execute(
                    "SELECT id FROM jobs WHERE synced=1 AND id NOT IN "
                    "(SELECT id FROM jobs ORDER BY updated DESC LIMIT 50)",
                )
            ]
            for rid in ids:
                db.execute("DELETE FROM jobs WHERE id=? AND synced=1", (rid,))
                for suffix in (".jpg", ".screen.jpg", ".preview.jpg"):
                    (self.root / (rid + suffix)).unlink(missing_ok=True)


class closing_connection:
    def __init__(self, db):
        self.db = db

    def __enter__(self):
        return self.db

    def __exit__(self, kind, value, tb):
        try:
            if kind:
                self.db.rollback()
            else:
                self.db.commit()
        finally:
            self.db.close()
