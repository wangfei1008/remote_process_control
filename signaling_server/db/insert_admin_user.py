# 一次性脚本：插入/更新 admin 用户（与前端 MD5 + 服务端 SHA-256 规则一致）
import hashlib
import sqlite3
from pathlib import Path

DB = Path(__file__).resolve().parent / "manage.db"
PLAIN = "Aa_123456"

md5_hex = hashlib.md5(PLAIN.encode("utf-8")).hexdigest()
password_hash = hashlib.sha256(md5_hex.encode("ascii")).hexdigest()

conn = sqlite3.connect(str(DB))
conn.execute(
    """INSERT INTO users (username, password_hash, role)
       VALUES ('admin', ?, 'Admin')
       ON CONFLICT(username) DO UPDATE SET
         password_hash = excluded.password_hash,
         role = excluded.role""",
    (password_hash,),
)
conn.commit()
row = conn.execute(
    "SELECT user_id, username, role, substr(password_hash,1,16) || '…' FROM users WHERE username=?",
    ("admin",),
).fetchone()
conn.close()
print("OK:", row)
print("md5_hex:", md5_hex)
print("password_hash:", password_hash)
