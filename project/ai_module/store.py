"""SQLite riêng của module AI: hạn mức lượt gọi mỗi ngày, nhật ký (không lưu ảnh) và thư viện
câu AI đã tạo (câu + giọng) để phát lại khi test hoặc khi Gemini hết lượt.
"""

import hashlib
import random
import sqlite3
import threading
import time
from contextlib import closing
from datetime import date


class Store:
    def __init__(self, path, library_size=30):
        self.path = path
        self.library_dir = path.parent / "library"
        self.library_size = library_size
        self.lock = threading.Lock()
        path.parent.mkdir(parents=True, exist_ok=True)
        with closing(self.connect()) as db, db:
            db.execute("CREATE TABLE IF NOT EXISTS usage(day TEXT PRIMARY KEY, count INTEGER)")
            db.execute(
                "CREATE TABLE IF NOT EXISTS log(id INTEGER PRIMARY KEY, time REAL, request_id TEXT,"
                " source TEXT, finger_count INTEGER, image_sent INTEGER, ok INTEGER, error TEXT,"
                " latency_ms INTEGER, text TEXT)"
            )
            db.execute(
                "CREATE TABLE IF NOT EXISTS library(id INTEGER PRIMARY KEY, finger_count INTEGER,"
                " text TEXT, wav TEXT, model TEXT, time REAL, UNIQUE(finger_count, text))"
            )
            columns = {row[1] for row in db.execute("PRAGMA table_info(log)")}
            if "model" not in columns:
                db.execute("ALTER TABLE log ADD COLUMN model TEXT")

    def connect(self):
        db = sqlite3.connect(self.path, timeout=5)
        db.row_factory = sqlite3.Row
        return db

    def take(self, limit):
        """Giữ 1 lượt trong hạn mức hôm nay; False nếu đã hết."""
        today = date.today().isoformat()
        with self.lock, closing(self.connect()) as db, db:
            row = db.execute("SELECT count FROM usage WHERE day=?", (today,)).fetchone()
            used = row["count"] if row else 0
            if used >= limit:
                return False
            db.execute(
                "INSERT INTO usage(day,count) VALUES(?,1) ON CONFLICT(day) DO UPDATE SET count=count+1",
                (today,),
            )
            return True

    def used_today(self):
        with closing(self.connect()) as db:
            row = db.execute(
                "SELECT count FROM usage WHERE day=?", (date.today().isoformat(),)
            ).fetchone()
            return row["count"] if row else 0

    def log(
        self, request_id, source, finger_count, image_sent, ok, error, latency_ms, text, model=None
    ):
        with self.lock, closing(self.connect()) as db, db:
            db.execute(
                "INSERT INTO log(time,request_id,source,finger_count,image_sent,ok,error,latency_ms,text,model)"
                " VALUES(?,?,?,?,?,?,?,?,?,?)",
                (
                    time.time(),
                    request_id,
                    source,
                    finger_count,
                    int(image_sent),
                    int(ok),
                    error,
                    latency_ms,
                    text,
                    model,
                ),
            )
            db.execute(
                "DELETE FROM log WHERE id NOT IN (SELECT id FROM log ORDER BY id DESC LIMIT 2000)"
            )

    def recent(self, limit=20):
        with closing(self.connect()) as db:
            rows = db.execute(
                "SELECT time,request_id,source,finger_count,image_sent,ok,error,latency_ms,text,model"
                " FROM log ORDER BY id DESC LIMIT ?",
                (limit,),
            ).fetchall()
            return [dict(row) for row in rows]

    # ---------- Thư viện phát lại ----------
    def remember(self, finger_count, text, wav, model):
        """Lưu câu + giọng của một lần chạy thật; giữ tối đa library_size câu mỗi số ngón."""
        self.library_dir.mkdir(parents=True, exist_ok=True)
        name = hashlib.sha256(wav).hexdigest() + ".wav"
        target = self.library_dir / name
        if not target.is_file():
            partial = target.with_suffix(".part")
            partial.write_bytes(wav)
            partial.replace(target)
        with self.lock, closing(self.connect()) as db, db:
            db.execute(
                "INSERT OR IGNORE INTO library(finger_count,text,wav,model,time) VALUES(?,?,?,?,?)",
                (finger_count, text, name, model, time.time()),
            )
            db.execute(
                "DELETE FROM library WHERE finger_count=? AND id NOT IN (SELECT id FROM library"
                " WHERE finger_count=? ORDER BY id DESC LIMIT ?)",
                (finger_count, finger_count, self.library_size),
            )

    def pick(self, finger_count):
        """Một câu ngẫu nhiên đã lưu cho số ngón này -> (text, wav, model) hoặc None."""
        with closing(self.connect()) as db:
            rows = db.execute(
                "SELECT text, wav, model FROM library WHERE finger_count=?", (finger_count,)
            ).fetchall()
        random.shuffle(rows)
        for row in rows:
            path = self.library_dir / row["wav"]
            if path.is_file():
                return row["text"], path.read_bytes(), row["model"]
        return None

    def library_counts(self):
        with closing(self.connect()) as db:
            rows = db.execute(
                "SELECT finger_count, COUNT(*) AS n FROM library GROUP BY finger_count"
            ).fetchall()
        return {row["finger_count"]: row["n"] for row in rows}
