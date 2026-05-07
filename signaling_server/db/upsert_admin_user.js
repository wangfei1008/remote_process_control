/**
 * 使用与前端一致的规则写入 admin：password_hash = SHA256( MD5(明文) 的十六进制串 )
 * 依赖：同目录下 npm install sql.js（见 .npm_sqljs）
 */
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const DB_PATH = path.join(__dirname, 'manage.db');
const PLAIN = 'Aa_123456';

const md5Hex = crypto.createHash('md5').update(PLAIN, 'utf8').digest('hex');
const passwordHash = crypto.createHash('sha256').update(md5Hex, 'utf8').digest('hex');

const initSqlJs = require(path.join(__dirname, '.npm_sqljs', 'node_modules', 'sql.js'));

(async function () {
    const SQL = await initSqlJs();
    const filebuffer = fs.readFileSync(DB_PATH);
    const db = new SQL.Database(filebuffer);
    db.run(
        `INSERT INTO users (username, password_hash, role)
         VALUES ('admin', ?, 'Admin')
         ON CONFLICT(username) DO UPDATE SET
           password_hash = excluded.password_hash,
           role = excluded.role`,
        [passwordHash]
    );
    const verify = db.exec("SELECT user_id, username, role FROM users WHERE username='admin'");
    const data = db.export();
    fs.writeFileSync(DB_PATH, Buffer.from(data));
    db.close();
    console.log('admin user:', verify);
    console.log('md5_hex:', md5Hex);
    console.log('password_hash:', passwordHash);
})().catch(function (e) {
    console.error(e);
    process.exit(1);
});
