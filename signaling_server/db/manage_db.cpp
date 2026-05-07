////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： manage.db 数据库访问封装实现（SQLite）
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
//详细功能说明：
// - 使用 sqlite3 预处理语句完成 CRUD
// - 文本时间字段以字符串形式绑定（与 DATETIME 存储一致）
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "manage_db.h"

#include "sqlite3.h"

namespace {

int map_sqlite_step(int rc)
{
	if (rc == SQLITE_ROW || rc == SQLITE_DONE)
		return manage_db_ok;
	if (rc == SQLITE_CONSTRAINT)
		return manage_db_err_constraint;
	return manage_db_err_sql;
}

void bind_text_or_null(sqlite3_stmt *st, int idx, const std::string &s, bool as_null_if_empty)
{
	if (as_null_if_empty && s.empty())
		sqlite3_bind_null(st, idx);
	else
		sqlite3_bind_text(st, idx, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
}

std::string column_text_str(sqlite3_stmt *st, int idx)
{
	if (sqlite3_column_type(st, idx) == SQLITE_NULL)
		return {};
	const unsigned char *t = sqlite3_column_text(st, idx);
	if (!t)
		return {};
	return std::string(reinterpret_cast<const char *>(t));
}

} // namespace

manage_db::manage_db()
	: m_db(nullptr)
{
}

manage_db::~manage_db()
{
	close();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          打开 SQLite 数据库文件
/// @参数
///          db_path--数据库路径（UTF-8）
/// @返回值 正常值。0:成功，小于0对应错误码
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
int manage_db::open(const char *db_path)
{
	close();
	if (!db_path || !db_path[0])
		return manage_db_err_invalid_arg;
	sqlite3 *db = nullptr;
	const int rc = sqlite3_open(db_path, &db);
	m_db = db;
	if (rc != SQLITE_OK) {
		if (m_db)
			sqlite3_close(m_db);
		m_db = nullptr;
		return manage_db_err_open;
	}
	sqlite3_busy_timeout(m_db, 3000);
	m_last_stmt_errmsg.clear();
	m_last_stmt_extended_errcode = 0;
	m_last_stmt_step_rc = 0;
	return manage_db_ok;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          关闭数据库连接
/// @参数
///          无
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
void manage_db::close()
{
	if (m_db) {
		sqlite3_close(m_db);
		m_db = nullptr;
	}
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          是否已打开数据库
/// @参数
///          无
/// @返回值 true 已打开
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
bool manage_db::is_open() const
{
	return m_db != nullptr;
}

std::string manage_db::sqlite_last_errmsg() const
{
	return m_last_stmt_errmsg;
}

int manage_db::sqlite_extended_errcode() const
{
	return m_last_stmt_extended_errcode;
}

int manage_db::sqlite_last_step_return_code() const
{
	return m_last_stmt_step_rc;
}

// ---------- users ----------

int manage_db::user_insert(const user_row &row, std::int64_t *new_user_id)
{
	if (!m_db || !new_user_id)
		return manage_db_err_invalid_arg;
	const char *sql =
	    "INSERT INTO users(username, password_hash, role, created_at) VALUES(?,?,?,"
	    "COALESCE(NULLIF(?,''), CURRENT_TIMESTAMP))";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_text(st, 1, row.username.c_str(), static_cast<int>(row.username.size()),
	                  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, row.password_hash.c_str(), static_cast<int>(row.password_hash.size()),
	                  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, row.role.c_str(), static_cast<int>(row.role.size()), SQLITE_TRANSIENT);
	bind_text_or_null(st, 4, row.created_at, true);
	const int rc = sqlite3_step(st);
	m_last_stmt_step_rc = rc;
	if (rc != SQLITE_DONE) {
		// 必须在 finalize / 其它 SQL 之前快照：后续语句会清除 sqlite3_errmsg
		const char *em = m_db ? sqlite3_errmsg(m_db) : nullptr;
		m_last_stmt_errmsg = em ? std::string(em) : std::string();
		m_last_stmt_extended_errcode = m_db ? sqlite3_extended_errcode(m_db) : 0;
	} else {
		m_last_stmt_errmsg.clear();
		m_last_stmt_extended_errcode = 0;
	}
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	*new_user_id = sqlite3_last_insert_rowid(m_db);
	return manage_db_ok;
}

int manage_db::user_update(const user_row &row)
{
	if (!m_db || row.user_id <= 0)
		return manage_db_err_invalid_arg;
	const char *sql = "UPDATE users SET username=?, password_hash=?, role=? WHERE user_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_text(st, 1, row.username.c_str(), static_cast<int>(row.username.size()),
	                  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, row.password_hash.c_str(), static_cast<int>(row.password_hash.size()),
	                  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, row.role.c_str(), static_cast<int>(row.role.size()), SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 4, static_cast<sqlite3_int64>(row.user_id));
	const int rc = sqlite3_step(st);
	m_last_stmt_step_rc = rc;
	if (rc != SQLITE_DONE) {
		const char *em = m_db ? sqlite3_errmsg(m_db) : nullptr;
		m_last_stmt_errmsg = em ? std::string(em) : std::string();
		m_last_stmt_extended_errcode = m_db ? sqlite3_extended_errcode(m_db) : 0;
	} else {
		m_last_stmt_errmsg.clear();
		m_last_stmt_extended_errcode = 0;
	}
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	if (sqlite3_changes(m_db) == 0)
		return manage_db_err_not_found;
	return manage_db_ok;
}

int manage_db::user_delete(std::int64_t user_id)
{
	if (!m_db || user_id <= 0)
		return manage_db_err_invalid_arg;
	const char *sql = "DELETE FROM users WHERE user_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(user_id));
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	if (sqlite3_changes(m_db) == 0)
		return manage_db_err_not_found;
	return manage_db_ok;
}

int manage_db::user_select_by_id(std::int64_t user_id, user_row *out)
{
	if (!m_db || user_id <= 0 || !out)
		return manage_db_err_invalid_arg;
	const char *sql = "SELECT user_id, username, password_hash, role,"
	                  " COALESCE(created_at,'') FROM users WHERE user_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(user_id));
	const int rc = sqlite3_step(st);
	if (rc != SQLITE_ROW) {
		sqlite3_finalize(st);
		return rc == SQLITE_DONE ? manage_db_err_not_found : manage_db_err_sql;
	}
	out->user_id = sqlite3_column_int64(st, 0);
	out->username = column_text_str(st, 1);
	out->password_hash = column_text_str(st, 2);
	out->role = column_text_str(st, 3);
	out->created_at = column_text_str(st, 4);
	sqlite3_finalize(st);
	return manage_db_ok;
}

int manage_db::user_select_by_username(const std::string &username, user_row *out)
{
	if (!m_db || username.empty() || !out)
		return manage_db_err_invalid_arg;
	const char *sql = "SELECT user_id, username, password_hash, role,"
	                  " COALESCE(created_at,'') FROM users WHERE username=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_text(st, 1, username.c_str(), static_cast<int>(username.size()), SQLITE_TRANSIENT);
	const int rc = sqlite3_step(st);
	if (rc != SQLITE_ROW) {
		sqlite3_finalize(st);
		return rc == SQLITE_DONE ? manage_db_err_not_found : manage_db_err_sql;
	}
	out->user_id = sqlite3_column_int64(st, 0);
	out->username = column_text_str(st, 1);
	out->password_hash = column_text_str(st, 2);
	out->role = column_text_str(st, 3);
	out->created_at = column_text_str(st, 4);
	sqlite3_finalize(st);
	return manage_db_ok;
}

int manage_db::user_select_all(std::vector<user_row> *out)
{
	if (!m_db || !out)
		return manage_db_err_invalid_arg;
	out->clear();
	const char *sql = "SELECT user_id, username, password_hash, role,"
	                  " COALESCE(created_at,'') FROM users ORDER BY user_id";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	int rc = SQLITE_OK;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		user_row r;
		r.user_id = sqlite3_column_int64(st, 0);
		r.username = column_text_str(st, 1);
		r.password_hash = column_text_str(st, 2);
		r.role = column_text_str(st, 3);
		r.created_at = column_text_str(st, 4);
		out->push_back(std::move(r));
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE ? manage_db_ok : manage_db_err_sql;
}

// ---------- nodes ----------

int manage_db::node_insert(const node_row &row, std::int64_t *new_node_id)
{
	if (!m_db || !new_node_id)
		return manage_db_err_invalid_arg;
	const char *sql =
	    "INSERT INTO nodes(node_name, auth_key, is_online, last_seen, created_at) VALUES(?,?,?,"
	    "NULLIF(?,''), COALESCE(NULLIF(?,''), CURRENT_TIMESTAMP))";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_text(st, 1, row.node_name.c_str(), static_cast<int>(row.node_name.size()),
	                  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, row.auth_key.c_str(), static_cast<int>(row.auth_key.size()),
	                  SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 3, row.is_online);
	bind_text_or_null(st, 4, row.last_seen, true);
	bind_text_or_null(st, 5, row.created_at, true);
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	*new_node_id = sqlite3_last_insert_rowid(m_db);
	return manage_db_ok;
}

int manage_db::node_update(const node_row &row)
{
	if (!m_db || row.node_id <= 0)
		return manage_db_err_invalid_arg;
	const char *sql =
	    "UPDATE nodes SET node_name=?, auth_key=?, is_online=?, last_seen=NULLIF(?,'') WHERE "
	    "node_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_text(st, 1, row.node_name.c_str(), static_cast<int>(row.node_name.size()),
	                  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, row.auth_key.c_str(), static_cast<int>(row.auth_key.size()),
	                  SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 3, row.is_online);
	bind_text_or_null(st, 4, row.last_seen, true);
	sqlite3_bind_int64(st, 5, static_cast<sqlite3_int64>(row.node_id));
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	if (sqlite3_changes(m_db) == 0)
		return manage_db_err_not_found;
	return manage_db_ok;
}

int manage_db::node_delete(std::int64_t node_id)
{
	if (!m_db || node_id <= 0)
		return manage_db_err_invalid_arg;
	const char *sql = "DELETE FROM nodes WHERE node_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(node_id));
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	if (sqlite3_changes(m_db) == 0)
		return manage_db_err_not_found;
	return manage_db_ok;
}

int manage_db::node_select_by_id(std::int64_t node_id, node_row *out)
{
	if (!m_db || node_id <= 0 || !out)
		return manage_db_err_invalid_arg;
	const char *sql =
	    "SELECT node_id, node_name, auth_key, IFNULL(is_online,0), COALESCE(last_seen,''), "
	    "COALESCE(created_at,'') FROM nodes WHERE node_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(node_id));
	const int rc = sqlite3_step(st);
	if (rc != SQLITE_ROW) {
		sqlite3_finalize(st);
		return rc == SQLITE_DONE ? manage_db_err_not_found : manage_db_err_sql;
	}
	out->node_id = sqlite3_column_int64(st, 0);
	out->node_name = column_text_str(st, 1);
	out->auth_key = column_text_str(st, 2);
	out->is_online = sqlite3_column_int(st, 3);
	out->last_seen = column_text_str(st, 4);
	out->created_at = column_text_str(st, 5);
	sqlite3_finalize(st);
	return manage_db_ok;
}

int manage_db::node_select_by_name(const std::string &node_name, node_row *out)
{
	if (!m_db || node_name.empty() || !out)
		return manage_db_err_invalid_arg;
	const char *sql =
	    "SELECT node_id, node_name, auth_key, IFNULL(is_online,0), COALESCE(last_seen,''), "
	    "COALESCE(created_at,'') FROM nodes WHERE node_name=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_text(st, 1, node_name.c_str(), static_cast<int>(node_name.size()), SQLITE_TRANSIENT);
	const int rc = sqlite3_step(st);
	if (rc != SQLITE_ROW) {
		sqlite3_finalize(st);
		return rc == SQLITE_DONE ? manage_db_err_not_found : manage_db_err_sql;
	}
	out->node_id = sqlite3_column_int64(st, 0);
	out->node_name = column_text_str(st, 1);
	out->auth_key = column_text_str(st, 2);
	out->is_online = sqlite3_column_int(st, 3);
	out->last_seen = column_text_str(st, 4);
	out->created_at = column_text_str(st, 5);
	sqlite3_finalize(st);
	return manage_db_ok;
}

int manage_db::node_select_all(std::vector<node_row> *out)
{
	if (!m_db || !out)
		return manage_db_err_invalid_arg;
	out->clear();
	const char *sql =
	    "SELECT node_id, node_name, auth_key, IFNULL(is_online,0), COALESCE(last_seen,''), "
	    "COALESCE(created_at,'') FROM nodes ORDER BY node_id";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	int rc = SQLITE_OK;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		node_row r;
		r.node_id = sqlite3_column_int64(st, 0);
		r.node_name = column_text_str(st, 1);
		r.auth_key = column_text_str(st, 2);
		r.is_online = sqlite3_column_int(st, 3);
		r.last_seen = column_text_str(st, 4);
		r.created_at = column_text_str(st, 5);
		out->push_back(std::move(r));
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE ? manage_db_ok : manage_db_err_sql;
}

// ---------- applications ----------

int manage_db::application_insert(const application_row &row, std::int64_t *new_app_id)
{
	if (!m_db || !new_app_id || row.node_id <= 0)
		return manage_db_err_invalid_arg;
	const char *sql =
	    "INSERT INTO applications(node_id, display_name, exe_path, work_dir, icon_data, is_public) "
	    "VALUES(?,?,?,?,?,?)";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(row.node_id));
	sqlite3_bind_text(st, 2, row.display_name.c_str(), static_cast<int>(row.display_name.size()),
	                  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, row.exe_path.c_str(), static_cast<int>(row.exe_path.size()),
	                  SQLITE_TRANSIENT);
	bind_text_or_null(st, 4, row.work_dir, true);
	bind_text_or_null(st, 5, row.icon_data, true);
	sqlite3_bind_int(st, 6, row.is_public);
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	*new_app_id = sqlite3_last_insert_rowid(m_db);
	return manage_db_ok;
}

int manage_db::application_update(const application_row &row)
{
	if (!m_db || row.app_id <= 0 || row.node_id <= 0)
		return manage_db_err_invalid_arg;
	const char *sql =
	    "UPDATE applications SET node_id=?, display_name=?, exe_path=?, work_dir=NULLIF(?,''), "
	    "icon_data=NULLIF(?,''), is_public=? WHERE app_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(row.node_id));
	sqlite3_bind_text(st, 2, row.display_name.c_str(), static_cast<int>(row.display_name.size()),
	                  SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, row.exe_path.c_str(), static_cast<int>(row.exe_path.size()),
	                  SQLITE_TRANSIENT);
	bind_text_or_null(st, 4, row.work_dir, true);
	bind_text_or_null(st, 5, row.icon_data, true);
	sqlite3_bind_int(st, 6, row.is_public);
	sqlite3_bind_int64(st, 7, static_cast<sqlite3_int64>(row.app_id));
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	if (sqlite3_changes(m_db) == 0)
		return manage_db_err_not_found;
	return manage_db_ok;
}

int manage_db::application_delete(std::int64_t app_id)
{
	if (!m_db || app_id <= 0)
		return manage_db_err_invalid_arg;
	const char *sql = "DELETE FROM applications WHERE app_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(app_id));
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	if (sqlite3_changes(m_db) == 0)
		return manage_db_err_not_found;
	return manage_db_ok;
}

int manage_db::application_select_by_id(std::int64_t app_id, application_row *out)
{
	if (!m_db || app_id <= 0 || !out)
		return manage_db_err_invalid_arg;
	const char *sql =
	    "SELECT app_id, node_id, display_name, exe_path, COALESCE(work_dir,''), "
	    "COALESCE(icon_data,''), IFNULL(is_public,0) FROM applications WHERE app_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(app_id));
	const int rc = sqlite3_step(st);
	if (rc != SQLITE_ROW) {
		sqlite3_finalize(st);
		return rc == SQLITE_DONE ? manage_db_err_not_found : manage_db_err_sql;
	}
	out->app_id = sqlite3_column_int64(st, 0);
	out->node_id = sqlite3_column_int64(st, 1);
	out->display_name = column_text_str(st, 2);
	out->exe_path = column_text_str(st, 3);
	out->work_dir = column_text_str(st, 4);
	out->icon_data = column_text_str(st, 5);
	out->is_public = sqlite3_column_int(st, 6);
	sqlite3_finalize(st);
	return manage_db_ok;
}

int manage_db::application_select_by_node(std::int64_t node_id, std::vector<application_row> *out)
{
	if (!m_db || node_id <= 0 || !out)
		return manage_db_err_invalid_arg;
	out->clear();
	const char *sql =
	    "SELECT app_id, node_id, display_name, exe_path, COALESCE(work_dir,''), "
	    "COALESCE(icon_data,''), IFNULL(is_public,0) FROM applications WHERE node_id=? ORDER BY "
	    "app_id";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(node_id));
	int rc = SQLITE_OK;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		application_row r;
		r.app_id = sqlite3_column_int64(st, 0);
		r.node_id = sqlite3_column_int64(st, 1);
		r.display_name = column_text_str(st, 2);
		r.exe_path = column_text_str(st, 3);
		r.work_dir = column_text_str(st, 4);
		r.icon_data = column_text_str(st, 5);
		r.is_public = sqlite3_column_int(st, 6);
		out->push_back(std::move(r));
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE ? manage_db_ok : manage_db_err_sql;
}

// ---------- permissions ----------

int manage_db::permission_insert(const permission_row &row, std::int64_t *new_perm_id)
{
	if (!m_db || !new_perm_id || row.user_id <= 0 || row.app_id <= 0)
		return manage_db_err_invalid_arg;
	const char *sql = "INSERT INTO permissions(user_id, app_id) VALUES(?,?)";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(row.user_id));
	sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(row.app_id));
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	*new_perm_id = sqlite3_last_insert_rowid(m_db);
	return manage_db_ok;
}

int manage_db::permission_delete_by_id(std::int64_t perm_id)
{
	if (!m_db || perm_id <= 0)
		return manage_db_err_invalid_arg;
	const char *sql = "DELETE FROM permissions WHERE perm_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(perm_id));
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	if (sqlite3_changes(m_db) == 0)
		return manage_db_err_not_found;
	return manage_db_ok;
}

int manage_db::permission_delete_by_pair(std::int64_t user_id, std::int64_t app_id)
{
	if (!m_db || user_id <= 0 || app_id <= 0)
		return manage_db_err_invalid_arg;
	const char *sql = "DELETE FROM permissions WHERE user_id=? AND app_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(user_id));
	sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(app_id));
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	if (sqlite3_changes(m_db) == 0)
		return manage_db_err_not_found;
	return manage_db_ok;
}

int manage_db::permission_select_by_id(std::int64_t perm_id, permission_row *out)
{
	if (!m_db || perm_id <= 0 || !out)
		return manage_db_err_invalid_arg;
	const char *sql = "SELECT perm_id, user_id, app_id FROM permissions WHERE perm_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(perm_id));
	const int rc = sqlite3_step(st);
	if (rc != SQLITE_ROW) {
		sqlite3_finalize(st);
		return rc == SQLITE_DONE ? manage_db_err_not_found : manage_db_err_sql;
	}
	out->perm_id = sqlite3_column_int64(st, 0);
	out->user_id = sqlite3_column_int64(st, 1);
	out->app_id = sqlite3_column_int64(st, 2);
	sqlite3_finalize(st);
	return manage_db_ok;
}

int manage_db::permission_select_all(std::vector<permission_row> *out)
{
	if (!m_db || !out)
		return manage_db_err_invalid_arg;
	out->clear();
	const char *sql = "SELECT perm_id, user_id, app_id FROM permissions ORDER BY perm_id";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	int rc = SQLITE_OK;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		permission_row r;
		r.perm_id = sqlite3_column_int64(st, 0);
		r.user_id = sqlite3_column_int64(st, 1);
		r.app_id = sqlite3_column_int64(st, 2);
		out->push_back(std::move(r));
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE ? manage_db_ok : manage_db_err_sql;
}

// ---------- session_logs ----------

int manage_db::session_log_insert(const session_log_row &row)
{
	if (!m_db || row.session_id.empty() || row.user_id <= 0 || row.app_id <= 0 || row.node_id <= 0 ||
	    row.start_time.empty())
		return manage_db_err_invalid_arg;
	const char *sql =
	    "INSERT INTO session_logs(session_id, user_id, app_id, node_id, start_time, end_time, "
	    "duration, exit_reason) VALUES(?,?,?,?,?,?,?,?)";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_text(st, 1, row.session_id.c_str(), static_cast<int>(row.session_id.size()),
	                  SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(row.user_id));
	sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(row.app_id));
	sqlite3_bind_int64(st, 4, static_cast<sqlite3_int64>(row.node_id));
	sqlite3_bind_text(st, 5, row.start_time.c_str(), static_cast<int>(row.start_time.size()),
	                  SQLITE_TRANSIENT);
	bind_text_or_null(st, 6, row.end_time, true);
	if (row.duration != 0)
		sqlite3_bind_int64(st, 7, static_cast<sqlite3_int64>(row.duration));
	else
		sqlite3_bind_null(st, 7);
	bind_text_or_null(st, 8, row.exit_reason, true);
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	return manage_db_ok;
}

int manage_db::session_log_update_end(const std::string &session_id, const std::string &end_time,
                                      std::int64_t duration, const std::string &exit_reason)
{
	if (!m_db || session_id.empty())
		return manage_db_err_invalid_arg;
	const char *sql =
	    "UPDATE session_logs SET end_time=NULLIF(?,''), duration=?, exit_reason=NULLIF(?,'') WHERE "
	    "session_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	bind_text_or_null(st, 1, end_time, true);
	sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(duration));
	bind_text_or_null(st, 3, exit_reason, true);
	sqlite3_bind_text(st, 4, session_id.c_str(), static_cast<int>(session_id.size()),
	                  SQLITE_TRANSIENT);
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	if (sqlite3_changes(m_db) == 0)
		return manage_db_err_not_found;
	return manage_db_ok;
}

int manage_db::session_log_delete(const std::string &session_id)
{
	if (!m_db || session_id.empty())
		return manage_db_err_invalid_arg;
	const char *sql = "DELETE FROM session_logs WHERE session_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_text(st, 1, session_id.c_str(), static_cast<int>(session_id.size()),
	                  SQLITE_TRANSIENT);
	const int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return map_sqlite_step(rc);
	if (sqlite3_changes(m_db) == 0)
		return manage_db_err_not_found;
	return manage_db_ok;
}

int manage_db::session_log_select(const std::string &session_id, session_log_row *out)
{
	if (!m_db || session_id.empty() || !out)
		return manage_db_err_invalid_arg;
	const char *sql =
	    "SELECT session_id, user_id, app_id, node_id, start_time, COALESCE(end_time,''), "
	    "IFNULL(duration,0), COALESCE(exit_reason,'') FROM session_logs WHERE session_id=?";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	sqlite3_bind_text(st, 1, session_id.c_str(), static_cast<int>(session_id.size()),
	                  SQLITE_TRANSIENT);
	const int rc = sqlite3_step(st);
	if (rc != SQLITE_ROW) {
		sqlite3_finalize(st);
		return rc == SQLITE_DONE ? manage_db_err_not_found : manage_db_err_sql;
	}
	out->session_id = column_text_str(st, 0);
	out->user_id = sqlite3_column_int64(st, 1);
	out->app_id = sqlite3_column_int64(st, 2);
	out->node_id = sqlite3_column_int64(st, 3);
	out->start_time = column_text_str(st, 4);
	out->end_time = column_text_str(st, 5);
	out->duration = sqlite3_column_int64(st, 6);
	out->exit_reason = column_text_str(st, 7);
	sqlite3_finalize(st);
	return manage_db_ok;
}

int manage_db::session_log_select_all(std::vector<session_log_row> *out)
{
	if (!m_db || !out)
		return manage_db_err_invalid_arg;
	out->clear();
	const char *sql =
	    "SELECT session_id, user_id, app_id, node_id, start_time, COALESCE(end_time,''), "
	    "IFNULL(duration,0), COALESCE(exit_reason,'') FROM session_logs ORDER BY start_time";
	sqlite3_stmt *st = nullptr;
	if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
		return manage_db_err_sql;
	int rc = SQLITE_OK;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		session_log_row r;
		r.session_id = column_text_str(st, 0);
		r.user_id = sqlite3_column_int64(st, 1);
		r.app_id = sqlite3_column_int64(st, 2);
		r.node_id = sqlite3_column_int64(st, 3);
		r.start_time = column_text_str(st, 4);
		r.end_time = column_text_str(st, 5);
		r.duration = sqlite3_column_int64(st, 6);
		r.exit_reason = column_text_str(st, 7);
		out->push_back(std::move(r));
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE ? manage_db_ok : manage_db_err_sql;
}
