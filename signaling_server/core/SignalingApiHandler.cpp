////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 信令 JSON 业务处理实现（DB + 中继）
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "SignalingApiHandler.h"

#include "ClientRegistry.h"
#include "SignalingRelay.h"

#include "external_api/external_api_envelope_types.h"
#include "external_api/external_api_protocol_constants.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QSet>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUuid>
#include <QtWebSockets/QWebSocket>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace {

using namespace external_api_envelope_types;

QString json_text_for_console_log(const QString &json_text)
{
    const int max_chars = 4096;
    if (json_text.size() <= max_chars)
        return json_text;
    return json_text.left(max_chars)
           + QStringLiteral("…(截断，原长 %1 字符)").arg(json_text.size());
}

using namespace external_api_models;
using namespace external_api_protocol_constants;

bool find_user_row_case_insensitive(manage_db &mdb, const QString &needle, user_row *out)
{
    std::vector<user_row> all;
    if (mdb.user_select_all(&all) != manage_db_ok)
        return false;
    for (const auto &u : all) {
        const QString qu = QString::fromStdString(u.username);
        if (qu.compare(needle, Qt::CaseInsensitive) == 0) {
            if (out)
                *out = u;
            return true;
        }
    }
    return false;
}

/// 管控台常发小写 role；与库内 CHECK `role IN ('Admin','User')` 对齐。
QString normalize_user_role_for_storage(const QString &role_in)
{
    const QString r = role_in.trimmed();
    if (r.compare(QLatin1String("admin"), Qt::CaseInsensitive) == 0)
        return QLatin1String("Admin");
    if (r.compare(QLatin1String("user"), Qt::CaseInsensitive) == 0)
        return QLatin1String("User");
    return QString();
}

QString mutate_message_for_user_sqlite_constraint(const std::string &errmsg)
{
    const QString e = QString::fromStdString(errmsg);
    if (e.contains(QLatin1String("username"), Qt::CaseInsensitive) ||
        e.contains(QLatin1String("users.username"), Qt::CaseInsensitive))
        return QStringLiteral("username_exists");
    if (e.contains(QLatin1String("CHECK"), Qt::CaseInsensitive) &&
        e.contains(QLatin1String("role"), Qt::CaseInsensitive))
        return QStringLiteral("invalid_role");
    return QStringLiteral("constraint");
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          当前 UTC 毫秒时间戳（latency_pong）
/// @参数
///          无
/// @返回值 qint64
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
qint64 now_ms_epoch()
{
    return QDateTime::currentMSecsSinceEpoch();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          适合写入 manage.db 的时间字符串（本地时区）
/// @参数
///          无
/// @返回值 QString
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
QString current_local_sqlite_datetime()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          客户端口令 SHA-256 十六进制（与入库哈希比对）
/// @参数
///          plain--明文口令
/// @返回值 hex 小写字符串
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
QString sha256_hex_utf8(const QString &plain)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(plain.toUtf8(), QCryptographicHash::Sha256).toHex());
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          判断载荷是否为 32 位 MD5 十六进制（前端已对明文口令做 MD5 后再传输）
/// @参数
///          s--password 字段原始字符串
/// @返回值 true 表示按「MD5 hex → 再 SHA-256 入库比对」路径校验
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
bool rpc_password_payload_is_md5_hex(const QString &s)
{
    if (s.size() != 32)
        return false;
    for (const QChar &ch : s) {
        const bool hex = (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
                         (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')) ||
                         (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'));
        if (!hex)
            return false;
    }
    return true;
}

QString password_hash_for_storage(const QString &recv)
{
    if (rpc_password_payload_is_md5_hex(recv))
        return sha256_hex_utf8(recv.toLower());
    return sha256_hex_utf8(recv);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          是否走服务器业务分支（此类消息即使携带 id 字段也不做中继）
/// @参数
///          message_type--JSON type 字段
/// @返回值 bool
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
bool is_business_message_type(const QString &message_type)
{
    return message_type == k_type_login() || message_type == k_type_node_register() ||
           message_type == k_type_get_auth_apps() || message_type == k_type_get_apps_icons() ||
           message_type == k_type_admin_add_app() || message_type == k_type_session_request() ||
           message_type == k_type_get_admin_stats() || message_type == k_type_get_admin_directory() ||
           message_type == k_type_admin_mutate() || message_type == k_type_latency_ping();
}

template <class... lambdas>
struct overloaded_visitor : lambdas... {
    using lambdas::operator()...;
};

template <class... lambdas>
overloaded_visitor(lambdas...) -> overloaded_visitor<lambdas...>;

} // namespace

signaling_api_handler::signaling_api_handler(ClientRegistry *client_registry, SignalingRelay *signaling_relay)
    : m_client_registry(client_registry)
    , m_signaling_relay(signaling_relay)
{
}

int signaling_api_handler::open_database(const QString &db_path)
{
    return m_manage_db.open(db_path.toUtf8().constData());
}

std::optional<signaling_token_record> signaling_api_handler::resolve_token(const QString &token) const
{
    if (token.isEmpty())
        return std::nullopt;
    const auto it = m_token_table.constFind(token);
    if (it == m_token_table.cend())
        return std::nullopt;
    return it.value();
}

bool signaling_api_handler::user_can_access_application(qint64 user_id, const application_row &application)
{
    if (application.is_public)
        return true;
    std::vector<permission_row> perms;
    if (m_manage_db.permission_select_all(&perms) != manage_db_ok)
        return false;
    for (const permission_row &perm : perms) {
        if (perm.user_id == user_id && perm.app_id == application.app_id)
            return true;
    }
    return false;
}

void signaling_api_handler::send_json(QWebSocket *socket,
                                       const external_api_envelope_types::external_api_message_variant &message)
{
    if (!socket)
        return;
    const QString text = m_protocol_facade.serialize_to_json_text(message);
    qInfo().noquote() << QStringLiteral("[res]") << socket->objectName()
                      << json_text_for_console_log(text);
    socket->sendTextMessage(text);
    socket->flush();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          节点下线：若 client_id 对应 nodes.node_name 则置离线
/// @参数
///          client_id--连接登记 id
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
void signaling_api_handler::on_socket_disconnected(const QString &client_id)
{
    if (client_id.isEmpty())
        return;
    node_row node{};
    const std::string name_utf8 = client_id.toStdString();
    if (m_manage_db.node_select_by_name(name_utf8, &node) != manage_db_ok)
        return;
    node_row updated = node;
    updated.is_online = 0;
    updated.last_seen = current_local_sqlite_datetime().toStdString();
    if (m_manage_db.node_update(updated) != manage_db_ok)
        qWarning() << "signaling_api_handler: node_update offline failed" << client_id;
}

void signaling_api_handler::handle_text_message(QWebSocket *socket, const QString &message)
{
    if (!socket || !m_client_registry || !m_signaling_relay)
        return;

    const QByteArray utf8 = message.toUtf8();
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(utf8, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        return;

    const QJsonObject root = document.object();
    const QString type = root.value(QStringLiteral("type")).toString();
    const QString destination_id = root.value(QStringLiteral("id")).toString();

    if (is_business_message_type(type)) {
        const std::optional<external_api_message_variant> parsed = m_protocol_facade.parse_json_bytes(utf8);
        if (!parsed) {
            qWarning() << "signaling_api_handler: business parse failed" << type;
            return;
        }
        std::visit(
            overloaded_visitor{
            [this, socket](const envelope_login &envelope) { handle_envelope_login(socket, envelope); },
            [this, socket](const envelope_node_register &envelope) {
                handle_envelope_node_register(socket, envelope);
            },
            [this, socket](const envelope_get_auth_apps &envelope) {
                handle_envelope_get_auth_apps(socket, envelope);
            },
            [this, socket](const envelope_get_apps_icons &envelope) {
                handle_envelope_get_apps_icons(socket, envelope);
            },
            [this, socket](const envelope_admin_add_app_req &envelope) {
                handle_envelope_admin_add_app_req(socket, envelope);
            },
            [this, socket](const envelope_session_request &envelope) {
                handle_envelope_session_request(socket, envelope);
            },
            [this, socket](const envelope_get_admin_stats &envelope) {
                handle_envelope_get_admin_stats(socket, envelope);
            },
            [this, socket](const envelope_get_admin_directory &envelope) {
                handle_envelope_get_admin_directory(socket, envelope);
            },
            [this, socket](const envelope_admin_mutate_req &envelope) {
                handle_envelope_admin_mutate_req(socket, envelope);
            },
            [this, socket](const envelope_latency_ping &envelope) {
                handle_envelope_latency_ping(socket, envelope);
            },
            [](const auto &) {
                qWarning() << "signaling_api_handler: unexpected variant arm for business type dispatch";
            },
            },
            *parsed);
        return;
    }

    if (!destination_id.isEmpty()) {
        m_signaling_relay->relay_text_message(socket, message);
        return;
    }

    const std::optional<external_api_message_variant> parsed = m_protocol_facade.parse_json_bytes(utf8);
    if (!parsed) {
        qWarning() << "signaling_api_handler: unknown JSON without destination id";
        return;
    }

    std::visit(
        overloaded_visitor{
        [this, socket](const envelope_login &envelope) { handle_envelope_login(socket, envelope); },
        [this, socket](const envelope_node_register &envelope) {
            handle_envelope_node_register(socket, envelope);
        },
        [this, socket](const envelope_get_auth_apps &envelope) {
            handle_envelope_get_auth_apps(socket, envelope);
        },
        [this, socket](const envelope_get_apps_icons &envelope) {
            handle_envelope_get_apps_icons(socket, envelope);
        },
        [this, socket](const envelope_admin_add_app_req &envelope) {
            handle_envelope_admin_add_app_req(socket, envelope);
        },
        [this, socket](const envelope_session_request &envelope) {
            handle_envelope_session_request(socket, envelope);
        },
        [this, socket](const envelope_get_admin_stats &envelope) {
            handle_envelope_get_admin_stats(socket, envelope);
        },
        [this, socket](const envelope_get_admin_directory &envelope) {
            handle_envelope_get_admin_directory(socket, envelope);
        },
        [this, socket](const envelope_admin_mutate_req &envelope) {
            handle_envelope_admin_mutate_req(socket, envelope);
        },
        [this, socket](const envelope_latency_ping &envelope) {
            handle_envelope_latency_ping(socket, envelope);
        },
        [](const envelope_login_res &) {},
        [](const envelope_auth_apps_res &) {},
        [](const envelope_apps_icons_res &) {},
        [](const envelope_admin_add_app_res &) {},
        [](const envelope_agent_start_session &) {},
        [](const envelope_webrtc_offer &) {
            qWarning() << "signaling_api_handler: webrtc_offer missing id";
        },
        [](const envelope_session_status_change &) {},
        [](const envelope_admin_stats_res &) {},
        [](const envelope_admin_directory_res &) {},
        [](const envelope_admin_mutate_res &) {},
        [](const envelope_file_list_req &) {},
        [](const envelope_file_list_res &) {},
        [](const envelope_file_download_init &) {},
        [](const envelope_file_download_data &) {},
        [](const envelope_file_upload_chunk &) {},
        [](const envelope_file_upload_ack &) {},
        [](const envelope_control_action &) {},
        [](const envelope_latency_pong &) {},
        },
        *parsed);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          用户登录：校验 users 表并签发令牌
/// @参数
///          socket--连接；envelope--login 载荷
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
void signaling_api_handler::handle_envelope_login(QWebSocket *socket,
                                                  const external_api_envelope_types::envelope_login &envelope)
{
    envelope_login_res response;
    response.success = false;
    response.data.token.clear();
    response.data.role.clear();

    user_row row{};
    const int rc = m_manage_db.user_select_by_username(envelope.data.username.toStdString(), &row);
    if (rc != manage_db_ok) {
        send_json(socket, response);
        return;
    }
    const QString hash_expected = QString::fromStdString(row.password_hash);
    const QString recv = envelope.data.password;
    QString hash_actual;
    if (rpc_password_payload_is_md5_hex(recv))
        hash_actual = sha256_hex_utf8(recv.toLower());
    else
        hash_actual = sha256_hex_utf8(recv);
    if (hash_actual != hash_expected) {
        send_json(socket, response);
        return;
    }

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    signaling_token_record record;
    record.user_id = row.user_id;
    record.username = QString::fromStdString(row.username);
    record.role = QString::fromStdString(row.role);
    m_token_table.insert(token, record);

    response.success = true;
    response.data.token = token;
    response.data.role = record.role;
    send_json(socket, response);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          节点注册：校验 auth_key 并标记在线
/// @参数
///          socket--连接；envelope--node_register 载荷
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
void signaling_api_handler::handle_envelope_node_register(
    QWebSocket *socket, const external_api_envelope_types::envelope_node_register &envelope)
{
    Q_UNUSED(socket);
    node_row row{};
    if (m_manage_db.node_select_by_name(envelope.data.node_name.toStdString(), &row) != manage_db_ok) {
        qWarning() << "signaling_api_handler: node_register unknown node" << envelope.data.node_name;
        return;
    }
    if (QString::fromStdString(row.auth_key) != envelope.data.auth_key) {
        qWarning() << "signaling_api_handler: node_register auth failed" << envelope.data.node_name;
        return;
    }
    node_row updated = row;
    updated.is_online = 1;
    updated.last_seen = current_local_sqlite_datetime().toStdString();
    if (m_manage_db.node_update(updated) != manage_db_ok)
        qWarning() << "signaling_api_handler: node_register update failed" << envelope.data.node_name;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          返回当前用户可访问的应用列表（权限并集 + 公开应用）
/// @参数
///          socket--连接；envelope--get_auth_apps
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
void signaling_api_handler::handle_envelope_get_auth_apps(
    QWebSocket *socket, const external_api_envelope_types::envelope_get_auth_apps &envelope)
{
    envelope_auth_apps_res response;
    const std::optional<signaling_token_record> user = resolve_token(envelope.token);
    if (!user) {
        send_json(socket, response);
        return;
    }

    QHash<qint64, auth_app_item> dedup;
    std::vector<permission_row> perms;
    if (m_manage_db.permission_select_all(&perms) == manage_db_ok) {
        for (const permission_row &perm : perms) {
            if (perm.user_id != user->user_id)
                continue;
            application_row app{};
            if (m_manage_db.application_select_by_id(perm.app_id, &app) != manage_db_ok)
                continue;
            node_row node{};
            if (m_manage_db.node_select_by_id(app.node_id, &node) != manage_db_ok)
                continue;
            auth_app_item item;
            item.app_id = app.app_id;
            item.display_name = QString::fromStdString(app.display_name);
            item.node_name = QString::fromStdString(node.node_name);
            item.exe_path = QString::fromStdString(app.exe_path);
            dedup.insert(app.app_id, item);
        }
    }

    std::vector<node_row> nodes;
    if (m_manage_db.node_select_all(&nodes) == manage_db_ok) {
        for (const node_row &node : nodes) {
            std::vector<application_row> apps;
            if (m_manage_db.application_select_by_node(node.node_id, &apps) != manage_db_ok)
                continue;
            for (const application_row &app : apps) {
                if (!app.is_public)
                    continue;
                if (dedup.contains(app.app_id))
                    continue;
                auth_app_item item;
                item.app_id = app.app_id;
                item.display_name = QString::fromStdString(app.display_name);
                item.node_name = QString::fromStdString(node.node_name);
                item.exe_path = QString::fromStdString(app.exe_path);
                dedup.insert(app.app_id, item);
            }
        }
    }

    for (auto it = dedup.cbegin(); it != dedup.cend(); ++it)
        response.body.apps.append(it.value());
    send_json(socket, response);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          批量返回应用图标（须具备访问权限）
/// @参数
///          socket--连接；envelope--get_apps_icons
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
void signaling_api_handler::handle_envelope_get_apps_icons(
    QWebSocket *socket, const external_api_envelope_types::envelope_get_apps_icons &envelope)
{
    envelope_apps_icons_res response;
    const std::optional<signaling_token_record> user = resolve_token(envelope.token);
    if (!user) {
        send_json(socket, response);
        return;
    }

    for (qint64 app_id : envelope.app_ids) {
        application_row app{};
        if (m_manage_db.application_select_by_id(app_id, &app) != manage_db_ok)
            continue;
        if (!user_can_access_application(user->user_id, app))
            continue;
        app_icon_item item;
        item.app_id = app.app_id;
        item.icon_base64 = QString::fromStdString(app.icon_data);
        response.icons.append(item);
    }
    send_json(socket, response);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          管理员登记应用并入库
/// @参数
///          socket--连接；envelope--admin_add_app 请求
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
void signaling_api_handler::handle_envelope_admin_add_app_req(
    QWebSocket *socket, const external_api_envelope_types::envelope_admin_add_app_req &envelope)
{
    envelope_admin_add_app_res response;
    response.success = false;
    response.data.app_id = 0;
    response.data.role.clear();

    const std::optional<signaling_token_record> user = resolve_token(envelope.token);
    if (!user || user->role.compare(QStringLiteral("admin"), Qt::CaseInsensitive) != 0) {
        send_json(socket, response);
        return;
    }

    node_row node{};
    if (m_manage_db.node_select_by_name(envelope.data.node_name.toStdString(), &node) != manage_db_ok) {
        send_json(socket, response);
        return;
    }

    application_row row{};
    row.node_id = node.node_id;
    row.display_name = envelope.data.display_name.toStdString();
    row.exe_path = envelope.data.exe_path.toStdString();
    row.work_dir.clear();
    row.icon_data = envelope.data.icon_base64.toStdString();
    row.is_public = envelope.data.is_public;

    std::int64_t new_app_id = 0;
    if (m_manage_db.application_insert(row, &new_app_id) != manage_db_ok) {
        send_json(socket, response);
        return;
    }

    response.success = true;
    response.data.app_id = new_app_id;
    response.data.role = user->role;
    send_json(socket, response);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          会话请求：写入会话日志并向目标节点投递 agent_start_session
/// @参数
///          socket--连接；envelope--session_request
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
void signaling_api_handler::handle_envelope_session_request(
    QWebSocket *socket, const external_api_envelope_types::envelope_session_request &envelope)
{
    const std::optional<signaling_token_record> user = resolve_token(envelope.token);
    if (!user) {
        envelope_session_status_change err;
        err.data.status = QStringLiteral("error");
        err.data.message = QStringLiteral("unauthorized");
        err.data.timestamp = now_ms_epoch();
        send_json(socket, err);
        return;
    }

    application_row app{};
    if (m_manage_db.application_select_by_id(envelope.data.app_id, &app) != manage_db_ok) {
        envelope_session_status_change err;
        err.data.status = QStringLiteral("error");
        err.data.message = QStringLiteral("unknown_app");
        err.data.timestamp = now_ms_epoch();
        send_json(socket, err);
        return;
    }

    if (!user_can_access_application(user->user_id, app)) {
        envelope_session_status_change err;
        err.data.status = QStringLiteral("error");
        err.data.message = QStringLiteral("forbidden");
        err.data.timestamp = now_ms_epoch();
        send_json(socket, err);
        return;
    }

    node_row node{};
    if (m_manage_db.node_select_by_id(app.node_id, &node) != manage_db_ok) {
        envelope_session_status_change err;
        err.data.status = QStringLiteral("error");
        err.data.message = QStringLiteral("unknown_node");
        err.data.timestamp = now_ms_epoch();
        send_json(socket, err);
        return;
    }

    const QString node_client_id = QString::fromStdString(node.node_name);
    QWebSocket *agent_socket = m_client_registry->get(node_client_id);
    if (!agent_socket) {
        envelope_session_status_change err;
        err.data.status = QStringLiteral("error");
        err.data.message = QStringLiteral("node_offline");
        err.data.timestamp = now_ms_epoch();
        send_json(socket, err);
        return;
    }

    const QString session_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    envelope_agent_start_session downstream;
    downstream.data.exe_path = QString::fromStdString(app.exe_path);
    downstream.data.session_id = session_id;
    downstream.data.grace_period = 60;
    send_json(agent_socket, downstream);

    session_log_row log_row;
    log_row.session_id = session_id.toStdString();
    log_row.user_id = user->user_id;
    log_row.app_id = app.app_id;
    log_row.node_id = app.node_id;
    log_row.start_time = current_local_sqlite_datetime().toStdString();
    log_row.end_time.clear();
    log_row.duration = 0;
    log_row.exit_reason.clear();
    if (m_manage_db.session_log_insert(log_row) != manage_db_ok)
        qWarning() << "signaling_api_handler: session_log_insert failed" << session_id;

    envelope_session_status_change pending;
    pending.data.status = QStringLiteral("pending");
    pending.data.message = session_id;
    pending.data.timestamp = now_ms_epoch();
    send_json(socket, pending);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          管理端统计：在线节点、活跃会话估计、用量排行
/// @参数
///          socket--连接；envelope--get_admin_stats
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
void signaling_api_handler::handle_envelope_get_admin_stats(
    QWebSocket *socket, const external_api_envelope_types::envelope_get_admin_stats &envelope)
{
    envelope_admin_stats_res response;
    const std::optional<signaling_token_record> user = resolve_token(envelope.token);
    if (!user || user->role.compare(QStringLiteral("admin"), Qt::CaseInsensitive) != 0) {
        send_json(socket, response);
        return;
    }

    std::vector<session_log_row> logs;
    if (m_manage_db.session_log_select_all(&logs) != manage_db_ok) {
        send_json(socket, response);
        return;
    }

    int active_sessions = 0;
    std::unordered_map<std::int64_t, std::int64_t> usage_sum;
    for (const session_log_row &log : logs) {
        if (log.end_time.empty())
            ++active_sessions;
        if (log.duration > 0)
            usage_sum[log.app_id] += log.duration;
    }
    response.data.active_sessions = active_sessions;

    std::vector<node_row> nodes;
    if (m_manage_db.node_select_all(&nodes) == manage_db_ok) {
        for (const node_row &node : nodes) {
            if (node.is_online)
                response.data.nodes_online.append(QString::fromStdString(node.node_name));
        }
    }

    QVector<usage_rank_item> ranking;
    ranking.reserve(static_cast<int>(usage_sum.size()));
    for (const auto &pair : usage_sum) {
        usage_rank_item item;
        item.app_id = pair.first;
        item.total_time = pair.second;
        ranking.append(item);
    }
    std::sort(ranking.begin(), ranking.end(), [](const usage_rank_item &a, const usage_rank_item &b) {
        return a.total_time > b.total_time;
    });
    const int limit = std::min(10, static_cast<int>(ranking.size()));
    for (int i = 0; i < limit; ++i)
        response.data.usage_ranking.append(ranking.at(i));

    send_json(socket, response);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          管理员：节点 / 用户 / 应用 / 权限清单（管控台）
/// @参数
///          socket--连接；envelope--get_admin_directory
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
void signaling_api_handler::handle_envelope_get_admin_directory(
    QWebSocket *socket, const external_api_envelope_types::envelope_get_admin_directory &envelope)
{
    envelope_admin_directory_res response;
    const std::optional<signaling_token_record> user = resolve_token(envelope.token);
    if (!user || user->role.compare(QStringLiteral("admin"), Qt::CaseInsensitive) != 0) {
        send_json(socket, response);
        return;
    }

    std::vector<node_row> node_rows;
    if (m_manage_db.node_select_all(&node_rows) != manage_db_ok) {
        send_json(socket, response);
        return;
    }

    for (const node_row &n : node_rows) {
        admin_directory_node_item it;
        it.node_id = n.node_id;
        it.node_name = QString::fromStdString(n.node_name);
        it.is_online = n.is_online;
        it.last_seen = QString::fromStdString(n.last_seen);
        response.data.nodes.append(it);
    }

    std::vector<user_row> user_rows;
    if (m_manage_db.user_select_all(&user_rows) == manage_db_ok) {
        for (const user_row &u : user_rows) {
            admin_directory_user_item it;
            it.user_id = u.user_id;
            it.username = QString::fromStdString(u.username);
            it.role = QString::fromStdString(u.role);
            response.data.users.append(it);
        }
    }

    QSet<qint64> app_seen;
    for (const node_row &n : node_rows) {
        std::vector<application_row> apps;
        if (m_manage_db.application_select_by_node(n.node_id, &apps) != manage_db_ok)
            continue;
        for (const application_row &a : apps) {
            if (app_seen.contains(static_cast<qint64>(a.app_id)))
                continue;
            app_seen.insert(static_cast<qint64>(a.app_id));
            admin_directory_app_item it;
            it.app_id = a.app_id;
            it.node_id = a.node_id;
            it.node_name = QString::fromStdString(n.node_name);
            it.display_name = QString::fromStdString(a.display_name);
            it.exe_path = QString::fromStdString(a.exe_path);
            it.is_public = a.is_public;
            response.data.apps.append(it);
        }
    }

    std::vector<permission_row> perms;
    if (m_manage_db.permission_select_all(&perms) == manage_db_ok) {
        for (const permission_row &p : perms) {
            user_row ur{};
            application_row ar{};
            node_row nr{};
            if (m_manage_db.user_select_by_id(p.user_id, &ur) != manage_db_ok)
                continue;
            if (m_manage_db.application_select_by_id(p.app_id, &ar) != manage_db_ok)
                continue;
            if (m_manage_db.node_select_by_id(ar.node_id, &nr) != manage_db_ok)
                continue;
            admin_directory_perm_item it;
            it.perm_id = p.perm_id;
            it.user_id = p.user_id;
            it.username = QString::fromStdString(ur.username);
            it.app_id = p.app_id;
            it.app_display_name = QString::fromStdString(ar.display_name);
            it.node_name = QString::fromStdString(nr.node_name);
            response.data.permissions.append(it);
        }
    }

    send_json(socket, response);
}

void signaling_api_handler::handle_envelope_admin_mutate_req(
    QWebSocket *socket, const external_api_envelope_types::envelope_admin_mutate_req &envelope)
{
    envelope_admin_mutate_res out;
    out.success = false;
    out.message.clear();
    out.entity = envelope.data.entity.trimmed();
    out.id = 0;

    const std::optional<signaling_token_record> admin = resolve_token(envelope.token);
    if (!admin || admin->role.compare(QStringLiteral("admin"), Qt::CaseInsensitive) != 0) {
        out.message = QStringLiteral("forbidden");
        send_json(socket, out);
        return;
    }

    const QString entity = envelope.data.entity.trimmed().toLower();
    const QString action = envelope.data.action.trimmed().toLower();
    const QJsonObject p = envelope.data.payload;

    auto send_out = [&]() { send_json(socket, out); };

    if (entity == QLatin1String("node")) {
        if (action == QLatin1String("create")) {
            const QString nn = p.value(QStringLiteral("node_name")).toString().trimmed();
            const QString ak = p.value(QStringLiteral("auth_key")).toString();
            if (nn.isEmpty() || ak.isEmpty()) {
                out.message = QStringLiteral("invalid");
                send_out();
                return;
            }
            node_row row{};
            row.node_name = nn.toStdString();
            row.auth_key = ak.toStdString();
            row.is_online = 0;
            std::int64_t new_id = 0;
            if (m_manage_db.node_insert(row, &new_id) != manage_db_ok) {
                out.message = QStringLiteral("db_error");
                send_out();
                return;
            }
            out.success = true;
            out.id = new_id;
            send_out();
            return;
        }
        if (action == QLatin1String("update")) {
            const qint64 node_id = p.value(QStringLiteral("node_id")).toVariant().toLongLong();
            if (node_id <= 0) {
                out.message = QStringLiteral("invalid");
                send_out();
                return;
            }
            node_row cur{};
            if (m_manage_db.node_select_by_id(node_id, &cur) != manage_db_ok) {
                out.message = QStringLiteral("not_found");
                send_out();
                return;
            }
            if (p.contains(QStringLiteral("node_name"))) {
                const QString nn = p.value(QStringLiteral("node_name")).toString().trimmed();
                if (!nn.isEmpty())
                    cur.node_name = nn.toStdString();
            }
            const QString ak = p.value(QStringLiteral("auth_key")).toString();
            if (!ak.isEmpty())
                cur.auth_key = ak.toStdString();
            if (m_manage_db.node_update(cur) != manage_db_ok) {
                out.message = QStringLiteral("db_error");
                send_out();
                return;
            }
            out.success = true;
            out.id = node_id;
            send_out();
            return;
        }
        if (action == QLatin1String("delete")) {
            const qint64 node_id = p.value(QStringLiteral("node_id")).toVariant().toLongLong();
            if (node_id <= 0) {
                out.message = QStringLiteral("invalid");
                send_out();
                return;
            }
            const int rc = m_manage_db.node_delete(node_id);
            if (rc == manage_db_err_not_found) {
                out.message = QStringLiteral("not_found");
                send_out();
                return;
            }
            if (rc != manage_db_ok) {
                out.message = QStringLiteral("db_error");
                send_out();
                return;
            }
            out.success = true;
            out.id = node_id;
            send_out();
            return;
        }
        out.message = QStringLiteral("bad_action");
        send_out();
        return;
    }

    if (entity == QLatin1String("application")) {
        if (action == QLatin1String("create")) {
            node_row node{};
            const QString node_name = p.value(QStringLiteral("node_name")).toString().trimmed();
            if (m_manage_db.node_select_by_name(node_name.toStdString(), &node) != manage_db_ok) {
                out.message = QStringLiteral("unknown_node");
                send_out();
                return;
            }
            application_row row{};
            row.node_id = node.node_id;
            row.display_name = p.value(QStringLiteral("display_name")).toString().toStdString();
            row.exe_path = p.value(QStringLiteral("exe_path")).toString().toStdString();
            row.work_dir.clear();
            row.icon_data = p.value(QStringLiteral("icon_base64")).toString().toStdString();
            row.is_public = p.value(QStringLiteral("is_public")).toInt() != 0 ? 1 : 0;
            std::int64_t new_app_id = 0;
            if (m_manage_db.application_insert(row, &new_app_id) != manage_db_ok) {
                out.message = QStringLiteral("db_error");
                send_out();
                return;
            }
            out.success = true;
            out.id = new_app_id;
            send_out();
            return;
        }
        if (action == QLatin1String("update")) {
            const qint64 app_id = p.value(QStringLiteral("app_id")).toVariant().toLongLong();
            if (app_id <= 0) {
                out.message = QStringLiteral("invalid");
                send_out();
                return;
            }
            application_row row{};
            if (m_manage_db.application_select_by_id(app_id, &row) != manage_db_ok) {
                out.message = QStringLiteral("not_found");
                send_out();
                return;
            }
            const QString node_name = p.value(QStringLiteral("node_name")).toString().trimmed();
            if (!node_name.isEmpty()) {
                node_row node{};
                if (m_manage_db.node_select_by_name(node_name.toStdString(), &node) != manage_db_ok) {
                    out.message = QStringLiteral("unknown_node");
                    send_out();
                    return;
                }
                row.node_id = node.node_id;
            }
            if (p.contains(QStringLiteral("display_name")))
                row.display_name = p.value(QStringLiteral("display_name")).toString().toStdString();
            if (p.contains(QStringLiteral("exe_path")))
                row.exe_path = p.value(QStringLiteral("exe_path")).toString().toStdString();
            if (p.contains(QStringLiteral("icon_base64")))
                row.icon_data = p.value(QStringLiteral("icon_base64")).toString().toStdString();
            if (p.contains(QStringLiteral("is_public")))
                row.is_public = p.value(QStringLiteral("is_public")).toInt() != 0 ? 1 : 0;
            if (m_manage_db.application_update(row) != manage_db_ok) {
                out.message = QStringLiteral("db_error");
                send_out();
                return;
            }
            out.success = true;
            out.id = app_id;
            send_out();
            return;
        }
        if (action == QLatin1String("delete")) {
            const qint64 app_id = p.value(QStringLiteral("app_id")).toVariant().toLongLong();
            if (app_id <= 0) {
                out.message = QStringLiteral("invalid");
                send_out();
                return;
            }
            const int rc = m_manage_db.application_delete(app_id);
            if (rc == manage_db_err_not_found) {
                out.message = QStringLiteral("not_found");
                send_out();
                return;
            }
            if (rc != manage_db_ok) {
                out.message = QStringLiteral("db_error");
                send_out();
                return;
            }
            out.success = true;
            out.id = app_id;
            send_out();
            return;
        }
        out.message = QStringLiteral("bad_action");
        send_out();
        return;
    }

    if (entity == QLatin1String("user")) {
        if (action == QLatin1String("create")) {
            const QString username = p.value(QStringLiteral("username")).toString().trimmed();
            const QString password = p.value(QStringLiteral("password")).toString();
            const QString role = p.value(QStringLiteral("role")).toString().trimmed();
            const QString role_norm = normalize_user_role_for_storage(role);
            if (username.isEmpty() || password.isEmpty() || role.isEmpty()) {
                out.message = QStringLiteral("invalid");
                send_out();
                return;
            }
            if (role_norm.isEmpty()) {
                out.message = QStringLiteral("invalid_role");
                send_out();
                return;
            }
            user_row dup{};
            if (m_manage_db.user_select_by_username(username.toStdString(), &dup) == manage_db_ok) {
                out.message = QStringLiteral("username_exists uid=%1")
                                  .arg(static_cast<qint64>(dup.user_id));
                qWarning() << "user create: username already in DB" << username << "existing_user_id"
                           << dup.user_id;
                send_out();
                return;
            }
            user_row row{};
            row.username = username.toStdString();
            row.password_hash = password_hash_for_storage(password).toStdString();
            row.role = role_norm.toStdString();
            std::int64_t new_id = 0;
            const int rc = m_manage_db.user_insert(row, &new_id);
            if (rc == manage_db_err_constraint) {
                user_row again{};
                if (m_manage_db.user_select_by_username(username.toStdString(), &again) == manage_db_ok) {
                    out.message = QStringLiteral("username_exists uid=%1")
                                      .arg(static_cast<qint64>(again.user_id));
                } else if (find_user_row_case_insensitive(m_manage_db, username, &again)) {
                    out.message = QStringLiteral("username_exists uid=%1")
                                      .arg(static_cast<qint64>(again.user_id));
                    qWarning() << "user insert UNIQUE failed; exact SELECT missed but case-insensitive match:"
                               << "requested=" << username
                               << "stored=" << QString::fromStdString(again.username) << "uid=" << again.user_id;
                } else {
                    out.message = mutate_message_for_user_sqlite_constraint(m_manage_db.sqlite_last_errmsg());
                    qWarning() << "user insert CONSTRAINT, step_rc=" << m_manage_db.sqlite_last_step_return_code()
                               << "sqlite_errmsg=" << QString::fromStdString(m_manage_db.sqlite_last_errmsg())
                               << "extended=" << m_manage_db.sqlite_extended_errcode()
                               << "wanted_username=" << username;
                    std::vector<user_row> all;
                    if (m_manage_db.user_select_all(&all) == manage_db_ok) {
                        qWarning() << "users table row count" << all.size();
                        for (const auto &u : all) {
                            qWarning() << "  uid=" << u.user_id
                                       << "username=" << QString::fromStdString(u.username);
                        }
                    }
                }
                qWarning() << "user insert constraint result message" << out.message;
                send_out();
                return;
            }
            if (rc != manage_db_ok) {
                out.message = QStringLiteral("db_error");
                send_out();
                return;
            }
            out.success = true;
            out.id = new_id;
            send_out();
            return;
        }
        if (action == QLatin1String("update")) {
            const qint64 user_id = p.value(QStringLiteral("user_id")).toVariant().toLongLong();
            if (user_id <= 0) {
                out.message = QStringLiteral("invalid");
                send_out();
                return;
            }
            user_row cur{};
            if (m_manage_db.user_select_by_id(user_id, &cur) != manage_db_ok) {
                out.message = QStringLiteral("not_found");
                send_out();
                return;
            }
            if (p.contains(QStringLiteral("username")))
                cur.username = p.value(QStringLiteral("username")).toString().trimmed().toStdString();
            if (p.contains(QStringLiteral("role"))) {
                const QString rn =
                    normalize_user_role_for_storage(p.value(QStringLiteral("role")).toString().trimmed());
                if (rn.isEmpty()) {
                    out.message = QStringLiteral("invalid_role");
                    send_out();
                    return;
                }
                cur.role = rn.toStdString();
            }
            const QString password = p.value(QStringLiteral("password")).toString();
            if (!password.isEmpty())
                cur.password_hash = password_hash_for_storage(password).toStdString();
            const int rc = m_manage_db.user_update(cur);
            if (rc == manage_db_err_not_found) {
                out.message = QStringLiteral("not_found");
                send_out();
                return;
            }
            if (rc == manage_db_err_constraint) {
                out.message = mutate_message_for_user_sqlite_constraint(m_manage_db.sqlite_last_errmsg());
                send_out();
                return;
            }
            if (rc != manage_db_ok) {
                out.message = QStringLiteral("db_error");
                send_out();
                return;
            }
            out.success = true;
            out.id = user_id;
            send_out();
            return;
        }
        if (action == QLatin1String("delete")) {
            const qint64 user_id = p.value(QStringLiteral("user_id")).toVariant().toLongLong();
            if (user_id <= 0) {
                out.message = QStringLiteral("invalid");
                send_out();
                return;
            }
            if (user_id == admin->user_id) {
                out.message = QStringLiteral("cannot_delete_self");
                send_out();
                return;
            }
            const int rc = m_manage_db.user_delete(user_id);
            if (rc == manage_db_err_not_found) {
                out.message = QStringLiteral("not_found");
                send_out();
                return;
            }
            if (rc != manage_db_ok) {
                out.message = QStringLiteral("db_error");
                send_out();
                return;
            }
            out.success = true;
            out.id = user_id;
            send_out();
            return;
        }
        out.message = QStringLiteral("bad_action");
        send_out();
        return;
    }

    if (entity == QLatin1String("permission")) {
        if (action == QLatin1String("create")) {
            const qint64 user_id = p.value(QStringLiteral("user_id")).toVariant().toLongLong();
            const qint64 app_id = p.value(QStringLiteral("app_id")).toVariant().toLongLong();
            if (user_id <= 0 || app_id <= 0) {
                out.message = QStringLiteral("invalid");
                send_out();
                return;
            }
            permission_row pr{};
            pr.user_id = user_id;
            pr.app_id = app_id;
            std::int64_t new_id = 0;
            const int rc = m_manage_db.permission_insert(pr, &new_id);
            if (rc == manage_db_err_constraint) {
                out.message = QStringLiteral("perm_exists");
                send_out();
                return;
            }
            if (rc != manage_db_ok) {
                out.message = QStringLiteral("db_error");
                send_out();
                return;
            }
            out.success = true;
            out.id = new_id;
            send_out();
            return;
        }
        if (action == QLatin1String("update")) {
            const qint64 perm_id = p.value(QStringLiteral("perm_id")).toVariant().toLongLong();
            const qint64 user_id = p.value(QStringLiteral("user_id")).toVariant().toLongLong();
            const qint64 app_id = p.value(QStringLiteral("app_id")).toVariant().toLongLong();
            if (perm_id <= 0 || user_id <= 0 || app_id <= 0) {
                out.message = QStringLiteral("invalid");
                send_out();
                return;
            }
            if (m_manage_db.permission_delete_by_id(perm_id) != manage_db_ok) {
                out.message = QStringLiteral("not_found");
                send_out();
                return;
            }
            permission_row pr{};
            pr.user_id = user_id;
            pr.app_id = app_id;
            std::int64_t new_id = 0;
            const int rc = m_manage_db.permission_insert(pr, &new_id);
            if (rc == manage_db_err_constraint) {
                out.message = QStringLiteral("perm_exists");
                send_out();
                return;
            }
            if (rc != manage_db_ok) {
                out.message = QStringLiteral("db_error");
                send_out();
                return;
            }
            out.success = true;
            out.id = new_id;
            send_out();
            return;
        }
        if (action == QLatin1String("delete")) {
            const qint64 perm_id = p.value(QStringLiteral("perm_id")).toVariant().toLongLong();
            if (perm_id <= 0) {
                out.message = QStringLiteral("invalid");
                send_out();
                return;
            }
            const int rc = m_manage_db.permission_delete_by_id(perm_id);
            if (rc == manage_db_err_not_found) {
                out.message = QStringLiteral("not_found");
                send_out();
                return;
            }
            if (rc != manage_db_ok) {
                out.message = QStringLiteral("db_error");
                send_out();
                return;
            }
            out.success = true;
            out.id = perm_id;
            send_out();
            return;
        }
        out.message = QStringLiteral("bad_action");
        send_out();
        return;
    }

    out.message = QStringLiteral("bad_entity");
    send_out();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          WebSocket 侧延迟探测应答（与 DataChannel 协议同名）
/// @参数
///          socket--连接；envelope--latency_ping
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
void signaling_api_handler::handle_envelope_latency_ping(QWebSocket *socket,
                                                         const external_api_envelope_types::envelope_latency_ping &envelope)
{
    envelope_latency_pong pong;
    pong.data.client_ts = envelope.data.client_ts;
    pong.data.server_ts = now_ms_epoch();
    pong.data.seq = envelope.data.seq;
    send_json(socket, pong);
}
