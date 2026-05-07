////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 对外接口协议门面实现 — JSON 编解码
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
//详细功能说明：
// - 实现《接口.docx》所列字段的解析与生成
// - 使用 std::visit 序列化各 variant 分支，便于扩展新 arm
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "external_api_protocol_facade.h"

#include "external_api_protocol_constants.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace {

using namespace external_api_envelope_types;
using namespace external_api_models;
using namespace external_api_protocol_constants;

/////////////////////////////////////////////////////////////////////////////
/// @说明 
///          从对象读取 qint64（兼容 JSON number）
/// @参数
///          object — JSON 对象
///          key — 字段名
/// @返回值 整数值
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
qint64 json_object_to_int64(const QJsonObject &object, const QString &key)
{
	return object.value(key).toVariant().toLongLong();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明 
///          解析授权应用列表项
/// @参数
///          object — 单项 JSON 对象
/// @返回值 auth_app_item
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
auth_app_item parse_auth_app_item(const QJsonObject &object)
{
	auth_app_item item;
	item.app_id = json_object_to_int64(object, QStringLiteral("app_id"));
	item.display_name = object.value(QStringLiteral("display_name")).toString();
	item.node_name = object.value(QStringLiteral("node_name")).toString();
	item.exe_path = object.value(QStringLiteral("exe_path")).toString();
	return item;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明 
///          解析文件条目
/// @参数
///          object — 单项 JSON 对象
/// @返回值 file_entry
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
file_entry parse_file_entry(const QJsonObject &object)
{
	file_entry entry;
	entry.name = object.value(QStringLiteral("name")).toString();
	entry.is_dir = object.value(QStringLiteral("is_dir")).toBool();
	entry.size = json_object_to_int64(object, QStringLiteral("size"));
	entry.mtime = json_object_to_int64(object, QStringLiteral("mtime"));
	return entry;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明 
///          解析用量排行项
/// @参数
///          object — 单项 JSON 对象
/// @返回值 usage_rank_item
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
usage_rank_item parse_usage_rank_item(const QJsonObject &object)
{
	usage_rank_item item;
	item.app_id = json_object_to_int64(object, QStringLiteral("app_id"));
	item.total_time = json_object_to_int64(object, QStringLiteral("total_time"));
	return item;
}

QJsonObject auth_app_item_to_json(const auth_app_item &item)
{
	QJsonObject object;
	object.insert(QStringLiteral("app_id"), static_cast<double>(item.app_id));
	object.insert(QStringLiteral("display_name"), item.display_name);
	object.insert(QStringLiteral("node_name"), item.node_name);
	object.insert(QStringLiteral("exe_path"), item.exe_path);
	return object;
}

QJsonObject app_icon_item_to_json(const app_icon_item &item)
{
	QJsonObject object;
	object.insert(QStringLiteral("app_id"), static_cast<double>(item.app_id));
	object.insert(QStringLiteral("icon_base64"), item.icon_base64);
	return object;
}

QJsonObject file_entry_to_json(const file_entry &entry)
{
	QJsonObject object;
	object.insert(QStringLiteral("name"), entry.name);
	object.insert(QStringLiteral("is_dir"), entry.is_dir);
	object.insert(QStringLiteral("size"), static_cast<double>(entry.size));
	object.insert(QStringLiteral("mtime"), static_cast<double>(entry.mtime));
	return object;
}

QJsonObject usage_rank_item_to_json(const usage_rank_item &item)
{
	QJsonObject object;
	object.insert(QStringLiteral("app_id"), static_cast<double>(item.app_id));
	object.insert(QStringLiteral("total_time"), static_cast<double>(item.total_time));
	return object;
}

QJsonObject admin_directory_node_item_to_json(const admin_directory_node_item &it)
{
	QJsonObject o;
	o.insert(QStringLiteral("node_id"), static_cast<double>(it.node_id));
	o.insert(QStringLiteral("node_name"), it.node_name);
	o.insert(QStringLiteral("is_online"), it.is_online);
	o.insert(QStringLiteral("last_seen"), it.last_seen);
	return o;
}

QJsonObject admin_directory_user_item_to_json(const admin_directory_user_item &it)
{
	QJsonObject o;
	o.insert(QStringLiteral("user_id"), static_cast<double>(it.user_id));
	o.insert(QStringLiteral("username"), it.username);
	o.insert(QStringLiteral("role"), it.role);
	return o;
}

QJsonObject admin_directory_app_item_to_json(const admin_directory_app_item &it)
{
	QJsonObject o;
	o.insert(QStringLiteral("app_id"), static_cast<double>(it.app_id));
	o.insert(QStringLiteral("node_id"), static_cast<double>(it.node_id));
	o.insert(QStringLiteral("node_name"), it.node_name);
	o.insert(QStringLiteral("display_name"), it.display_name);
	o.insert(QStringLiteral("exe_path"), it.exe_path);
	o.insert(QStringLiteral("is_public"), it.is_public);
	return o;
}

QJsonObject admin_directory_perm_item_to_json(const admin_directory_perm_item &it)
{
	QJsonObject o;
	o.insert(QStringLiteral("perm_id"), static_cast<double>(it.perm_id));
	o.insert(QStringLiteral("user_id"), static_cast<double>(it.user_id));
	o.insert(QStringLiteral("username"), it.username);
	o.insert(QStringLiteral("app_id"), static_cast<double>(it.app_id));
	o.insert(QStringLiteral("app_display_name"), it.app_display_name);
	o.insert(QStringLiteral("node_name"), it.node_name);
	return o;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明 
///          根据根对象 type 字段构造信封（工厂分支入口）
/// @参数
///          root — JSON 根对象
/// @返回值 optional 信封
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
std::optional<external_api_message_variant> parse_root_object(const QJsonObject &root)
{
	const QString type = root.value(QStringLiteral("type")).toString();
	const QJsonObject data_object = root.value(QStringLiteral("data")).toObject();

	if (type == k_type_login()) {
		envelope_login message;
		message.data.username = data_object.value(QStringLiteral("username")).toString();
		message.data.password = data_object.value(QStringLiteral("password")).toString();
		return message;
	}
	if (type == k_type_login_res()) {
		envelope_login_res message;
		message.success = root.value(QStringLiteral("success")).toBool();
		const QJsonObject inner = data_object;
		message.data.token = inner.value(QStringLiteral("token")).toString();
		message.data.role = inner.value(QStringLiteral("role")).toString();
		return message;
	}
	if (type == k_type_node_register()) {
		envelope_node_register message;
		message.data.node_name = data_object.value(QStringLiteral("node_name")).toString();
		message.data.auth_key = data_object.value(QStringLiteral("auth_key")).toString();
		return message;
	}
	if (type == k_type_get_auth_apps()) {
		envelope_get_auth_apps message;
		message.token = root.value(QStringLiteral("token")).toString();
		return message;
	}
	if (type == k_type_auth_apps_res()) {
		envelope_auth_apps_res message;
		const QJsonArray apps_array = root.value(QStringLiteral("apps")).toArray();
		for (const QJsonValue &value : apps_array) {
			message.body.apps.append(parse_auth_app_item(value.toObject()));
		}
		return message;
	}
	if (type == k_type_get_apps_icons()) {
		envelope_get_apps_icons message;
		message.token = root.value(QStringLiteral("token")).toString();
		const QJsonArray ids = root.value(QStringLiteral("app_ids")).toArray();
		for (const QJsonValue &value : ids) {
			message.app_ids.append(static_cast<qint64>(value.toVariant().toLongLong()));
		}
		return message;
	}
	if (type == k_type_apps_icons_res()) {
		envelope_apps_icons_res message;
		const QJsonArray icons_array = root.value(QStringLiteral("icons")).toArray();
		for (const QJsonValue &value : icons_array) {
			const QJsonObject object = value.toObject();
			app_icon_item item;
			item.app_id = json_object_to_int64(object, QStringLiteral("app_id"));
			item.icon_base64 = object.value(QStringLiteral("icon_base64")).toString();
			message.icons.append(item);
		}
		return message;
	}
	if (type == k_type_admin_add_app()) {
		if (root.contains(QStringLiteral("token"))) {
			envelope_admin_add_app_req message;
			message.token = root.value(QStringLiteral("token")).toString();
			message.data.node_name = data_object.value(QStringLiteral("node_name")).toString();
			message.data.display_name = data_object.value(QStringLiteral("display_name")).toString();
			message.data.exe_path = data_object.value(QStringLiteral("exe_path")).toString();
			message.data.icon_base64 = data_object.value(QStringLiteral("icon_base64")).toString();
			message.data.is_public = data_object.value(QStringLiteral("is_public")).toInt();
			return message;
		}
		envelope_admin_add_app_res message;
		message.success = root.value(QStringLiteral("success")).toBool();
		message.data.app_id = json_object_to_int64(data_object, QStringLiteral("app_id"));
		message.data.role = data_object.value(QStringLiteral("role")).toString();
		return message;
	}
	if (type == k_type_session_request()) {
		envelope_session_request message;
		message.token = root.value(QStringLiteral("token")).toString();
		message.data.app_id = json_object_to_int64(data_object, QStringLiteral("app_id"));
		message.data.is_reconnect = data_object.value(QStringLiteral("is_reconnect")).toBool();
		return message;
	}
	if (type == k_type_agent_start_session()) {
		envelope_agent_start_session message;
		message.data.exe_path = data_object.value(QStringLiteral("exe_path")).toString();
		message.data.session_id = data_object.value(QStringLiteral("session_id")).toString();
		message.data.grace_period = data_object.value(QStringLiteral("grace_period")).toInt();
		message.data.operator_client_id = data_object.value(QStringLiteral("operator_client_id")).toString();
		return message;
	}
	if (type == k_type_webrtc_offer()) {
		envelope_webrtc_offer message;
		message.data.sdp = data_object.value(QStringLiteral("sdp")).toString();
		message.data.session_id = data_object.value(QStringLiteral("session_id")).toString();
		return message;
	}
	if (type == k_type_session_status_change()) {
		envelope_session_status_change message;
		message.data.status = data_object.value(QStringLiteral("status")).toString();
		message.data.message = data_object.value(QStringLiteral("message")).toString();
		message.data.timestamp = json_object_to_int64(data_object, QStringLiteral("timestamp"));
		return message;
	}
	if (type == k_type_get_admin_stats()) {
		envelope_get_admin_stats message;
		message.token = root.value(QStringLiteral("token")).toString();
		return message;
	}
	if (type == k_type_admin_stats_res()) {
		envelope_admin_stats_res message;
		const QJsonObject stats_object = root.value(QStringLiteral("data")).toObject();
		message.data.active_sessions = stats_object.value(QStringLiteral("active_sessions")).toInt();
		const QJsonArray nodes = stats_object.value(QStringLiteral("nodes_online")).toArray();
		for (const QJsonValue &value : nodes) {
			message.data.nodes_online.append(value.toString());
		}
		const QJsonArray ranking = stats_object.value(QStringLiteral("usage_ranking")).toArray();
		for (const QJsonValue &value : ranking) {
			message.data.usage_ranking.append(parse_usage_rank_item(value.toObject()));
		}
		return message;
	}
	if (type == k_type_get_admin_directory()) {
		envelope_get_admin_directory message;
		message.token = root.value(QStringLiteral("token")).toString();
		return message;
	}
	if (type == k_type_admin_mutate()) {
		envelope_admin_mutate_req message;
		message.token = root.value(QStringLiteral("token")).toString();
		message.data.entity = data_object.value(QStringLiteral("entity")).toString();
		message.data.action = data_object.value(QStringLiteral("action")).toString();
		message.data.payload = data_object.value(QStringLiteral("payload")).toObject();
		return message;
	}
	if (type == k_type_admin_mutate_res()) {
		envelope_admin_mutate_res message;
		message.success = root.value(QStringLiteral("success")).toBool();
		message.message = root.value(QStringLiteral("message")).toString();
		const QJsonObject d = root.value(QStringLiteral("data")).toObject();
		message.entity = d.value(QStringLiteral("entity")).toString();
		message.id = json_object_to_int64(d, QStringLiteral("id"));
		return message;
	}
	if (type == k_type_admin_directory_res()) {
		envelope_admin_directory_res message;
		const QJsonObject d = root.value(QStringLiteral("data")).toObject();
		const QJsonArray na = d.value(QStringLiteral("nodes")).toArray();
		for (const QJsonValue &v : na) {
			const QJsonObject o = v.toObject();
			admin_directory_node_item it;
			it.node_id = json_object_to_int64(o, QStringLiteral("node_id"));
			it.node_name = o.value(QStringLiteral("node_name")).toString();
			it.is_online = o.value(QStringLiteral("is_online")).toInt();
			it.last_seen = o.value(QStringLiteral("last_seen")).toString();
			message.data.nodes.append(it);
		}
		const QJsonArray ua = d.value(QStringLiteral("users")).toArray();
		for (const QJsonValue &v : ua) {
			const QJsonObject o = v.toObject();
			admin_directory_user_item it;
			it.user_id = json_object_to_int64(o, QStringLiteral("user_id"));
			it.username = o.value(QStringLiteral("username")).toString();
			it.role = o.value(QStringLiteral("role")).toString();
			message.data.users.append(it);
		}
		const QJsonArray aa = d.value(QStringLiteral("apps")).toArray();
		for (const QJsonValue &v : aa) {
			const QJsonObject o = v.toObject();
			admin_directory_app_item it;
			it.app_id = json_object_to_int64(o, QStringLiteral("app_id"));
			it.node_id = json_object_to_int64(o, QStringLiteral("node_id"));
			it.node_name = o.value(QStringLiteral("node_name")).toString();
			it.display_name = o.value(QStringLiteral("display_name")).toString();
			it.exe_path = o.value(QStringLiteral("exe_path")).toString();
			it.is_public = o.value(QStringLiteral("is_public")).toInt();
			message.data.apps.append(it);
		}
		const QJsonArray pa = d.value(QStringLiteral("permissions")).toArray();
		for (const QJsonValue &v : pa) {
			const QJsonObject o = v.toObject();
			admin_directory_perm_item it;
			it.perm_id = json_object_to_int64(o, QStringLiteral("perm_id"));
			it.user_id = json_object_to_int64(o, QStringLiteral("user_id"));
			it.username = o.value(QStringLiteral("username")).toString();
			it.app_id = json_object_to_int64(o, QStringLiteral("app_id"));
			it.app_display_name = o.value(QStringLiteral("app_display_name")).toString();
			it.node_name = o.value(QStringLiteral("node_name")).toString();
			message.data.permissions.append(it);
		}
		return message;
	}
	if (type == k_type_file_list_req()) {
		envelope_file_list_req message;
		message.token = root.value(QStringLiteral("token")).toString();
		message.data.path = data_object.value(QStringLiteral("path")).toString();
		return message;
	}
	if (type == k_type_file_list_res()) {
		envelope_file_list_res message;
		message.data.current_path = data_object.value(QStringLiteral("current_path")).toString();
		const QJsonArray entries = data_object.value(QStringLiteral("entries")).toArray();
		for (const QJsonValue &value : entries) {
			message.data.entries.append(parse_file_entry(value.toObject()));
		}
		return message;
	}
	if (type == k_type_file_download_init()) {
		envelope_file_download_init message;
		message.data.path = data_object.value(QStringLiteral("path")).toString();
		message.data.chunk_size = data_object.value(QStringLiteral("chunk_size")).toInt(16384);
		return message;
	}
	if (type == k_type_file_download_data()) {
		envelope_file_download_data message;
		message.data.transfer_id = data_object.value(QStringLiteral("transfer_id")).toString();
		message.data.offset = json_object_to_int64(data_object, QStringLiteral("offset"));
		message.data.data_base64 = data_object.value(QStringLiteral("data")).toString();
		message.data.is_eof = data_object.value(QStringLiteral("is_eof")).toBool();
		return message;
	}
	if (type == k_type_file_upload_chunk()) {
		envelope_file_upload_chunk message;
		message.data.transfer_id = data_object.value(QStringLiteral("transfer_id")).toString();
		message.data.offset = json_object_to_int64(data_object, QStringLiteral("offset"));
		message.data.data_base64 = data_object.value(QStringLiteral("data")).toString();
		return message;
	}
	if (type == k_type_file_upload_ack()) {
		envelope_file_upload_ack message;
		message.success = root.value(QStringLiteral("success")).toBool();
		message.data.transfer_id = data_object.value(QStringLiteral("transfer_id")).toString();
		message.data.next_expected_offset = json_object_to_int64(data_object, QStringLiteral("next_expected_offset"));
		message.data.is_complete = data_object.value(QStringLiteral("is_complete")).toBool();
		return message;
	}
	if (type == k_type_control_action()) {
		envelope_control_action message;
		message.data.action = data_object.value(QStringLiteral("action")).toString();
		message.data.force = data_object.value(QStringLiteral("force")).toBool();
		return message;
	}
	if (type == k_type_latency_ping()) {
		envelope_latency_ping message;
		message.data.client_ts = json_object_to_int64(data_object, QStringLiteral("client_ts"));
		message.data.seq = data_object.value(QStringLiteral("seq")).toInt();
		return message;
	}
	if (type == k_type_latency_pong()) {
		envelope_latency_pong message;
		message.data.client_ts = json_object_to_int64(data_object, QStringLiteral("client_ts"));
		message.data.server_ts = json_object_to_int64(data_object, QStringLiteral("server_ts"));
		message.data.seq = data_object.value(QStringLiteral("seq")).toInt();
		return message;
	}
	return std::nullopt;
}

template <class... lambdas>
struct overloaded_visitor : lambdas... {
	using lambdas::operator()...;
};

template <class... lambdas>
overloaded_visitor(lambdas...) -> overloaded_visitor<lambdas...>;

} // namespace

std::optional<external_api_message_variant> external_api_protocol_facade::parse_json_bytes(const QByteArray &utf8_json) const
{
	QJsonParseError parse_error;
	const QJsonDocument document = QJsonDocument::fromJson(utf8_json, &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	return parse_root_object(document.object());
}

std::optional<external_api_message_variant> external_api_protocol_facade::parse_json_text(const QString &json_text) const
{
	return parse_json_bytes(json_text.toUtf8());
}

QByteArray external_api_protocol_facade::serialize_to_json_utf8(const external_api_message_variant &message) const
{
	QJsonObject root;
	std::visit(
	    overloaded_visitor{
		[&](const envelope_login &message) {
			root.insert(QStringLiteral("type"), k_type_login());
			QJsonObject data_object;
			data_object.insert(QStringLiteral("username"), message.data.username);
			data_object.insert(QStringLiteral("password"), message.data.password);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_login_res &message) {
			root.insert(QStringLiteral("type"), k_type_login_res());
			root.insert(QStringLiteral("success"), message.success);
			QJsonObject data_object;
			data_object.insert(QStringLiteral("token"), message.data.token);
			data_object.insert(QStringLiteral("role"), message.data.role);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_node_register &message) {
			root.insert(QStringLiteral("type"), k_type_node_register());
			QJsonObject data_object;
			data_object.insert(QStringLiteral("node_name"), message.data.node_name);
			data_object.insert(QStringLiteral("auth_key"), message.data.auth_key);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_get_auth_apps &message) {
			root.insert(QStringLiteral("type"), k_type_get_auth_apps());
			root.insert(QStringLiteral("token"), message.token);
		},
		[&](const envelope_auth_apps_res &message) {
			root.insert(QStringLiteral("type"), k_type_auth_apps_res());
			QJsonArray apps_array;
			for (const auth_app_item &item : message.body.apps) {
				apps_array.append(auth_app_item_to_json(item));
			}
			root.insert(QStringLiteral("apps"), apps_array);
		},
		[&](const envelope_get_apps_icons &message) {
			root.insert(QStringLiteral("type"), k_type_get_apps_icons());
			root.insert(QStringLiteral("token"), message.token);
			QJsonArray ids_array;
			for (qint64 id : message.app_ids) {
				ids_array.append(static_cast<double>(id));
			}
			root.insert(QStringLiteral("app_ids"), ids_array);
		},
		[&](const envelope_apps_icons_res &message) {
			root.insert(QStringLiteral("type"), k_type_apps_icons_res());
			QJsonArray icons_array;
			for (const app_icon_item &item : message.icons) {
				icons_array.append(app_icon_item_to_json(item));
			}
			root.insert(QStringLiteral("icons"), icons_array);
		},
		[&](const envelope_admin_add_app_req &message) {
			root.insert(QStringLiteral("type"), k_type_admin_add_app());
			root.insert(QStringLiteral("token"), message.token);
			QJsonObject data_object;
			data_object.insert(QStringLiteral("node_name"), message.data.node_name);
			data_object.insert(QStringLiteral("display_name"), message.data.display_name);
			data_object.insert(QStringLiteral("exe_path"), message.data.exe_path);
			data_object.insert(QStringLiteral("icon_base64"), message.data.icon_base64);
			data_object.insert(QStringLiteral("is_public"), message.data.is_public);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_admin_add_app_res &message) {
			root.insert(QStringLiteral("type"), k_type_admin_add_app());
			root.insert(QStringLiteral("success"), message.success);
			QJsonObject data_object;
			data_object.insert(QStringLiteral("app_id"), static_cast<double>(message.data.app_id));
			data_object.insert(QStringLiteral("role"), message.data.role);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_session_request &message) {
			root.insert(QStringLiteral("type"), k_type_session_request());
			root.insert(QStringLiteral("token"), message.token);
			QJsonObject data_object;
			data_object.insert(QStringLiteral("app_id"), static_cast<double>(message.data.app_id));
			data_object.insert(QStringLiteral("is_reconnect"), message.data.is_reconnect);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_agent_start_session &message) {
			root.insert(QStringLiteral("type"), k_type_agent_start_session());
			QJsonObject data_object;
			data_object.insert(QStringLiteral("exe_path"), message.data.exe_path);
			data_object.insert(QStringLiteral("session_id"), message.data.session_id);
			data_object.insert(QStringLiteral("grace_period"), message.data.grace_period);
			data_object.insert(QStringLiteral("operator_client_id"), message.data.operator_client_id);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_webrtc_offer &message) {
			root.insert(QStringLiteral("type"), k_type_webrtc_offer());
			QJsonObject data_object;
			data_object.insert(QStringLiteral("sdp"), message.data.sdp);
			data_object.insert(QStringLiteral("session_id"), message.data.session_id);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_session_status_change &message) {
			root.insert(QStringLiteral("type"), k_type_session_status_change());
			QJsonObject data_object;
			data_object.insert(QStringLiteral("status"), message.data.status);
			data_object.insert(QStringLiteral("message"), message.data.message);
			data_object.insert(QStringLiteral("timestamp"), static_cast<double>(message.data.timestamp));
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_get_admin_stats &message) {
			root.insert(QStringLiteral("type"), k_type_get_admin_stats());
			root.insert(QStringLiteral("token"), message.token);
		},
		[&](const envelope_admin_stats_res &message) {
			root.insert(QStringLiteral("type"), k_type_admin_stats_res());
			QJsonObject stats_object;
			stats_object.insert(QStringLiteral("active_sessions"), message.data.active_sessions);
			QJsonArray nodes_array;
			for (const QString &node : message.data.nodes_online) {
				nodes_array.append(node);
			}
			stats_object.insert(QStringLiteral("nodes_online"), nodes_array);
			QJsonArray ranking_array;
			for (const usage_rank_item &item : message.data.usage_ranking) {
				ranking_array.append(usage_rank_item_to_json(item));
			}
			stats_object.insert(QStringLiteral("usage_ranking"), ranking_array);
			root.insert(QStringLiteral("data"), stats_object);
		},
		[&](const envelope_get_admin_directory &message) {
			root.insert(QStringLiteral("type"), k_type_get_admin_directory());
			root.insert(QStringLiteral("token"), message.token);
		},
		[&](const envelope_admin_mutate_req &message) {
			root.insert(QStringLiteral("type"), k_type_admin_mutate());
			root.insert(QStringLiteral("token"), message.token);
			QJsonObject data_object;
			data_object.insert(QStringLiteral("entity"), message.data.entity);
			data_object.insert(QStringLiteral("action"), message.data.action);
			data_object.insert(QStringLiteral("payload"), message.data.payload);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_admin_mutate_res &message) {
			root.insert(QStringLiteral("type"), k_type_admin_mutate_res());
			root.insert(QStringLiteral("success"), message.success);
			root.insert(QStringLiteral("message"), message.message);
			QJsonObject data_object;
			data_object.insert(QStringLiteral("entity"), message.entity);
			data_object.insert(QStringLiteral("id"), static_cast<double>(message.id));
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_admin_directory_res &message) {
			root.insert(QStringLiteral("type"), k_type_admin_directory_res());
			QJsonObject d;
			QJsonArray na;
			for (const admin_directory_node_item &it : message.data.nodes)
				na.append(admin_directory_node_item_to_json(it));
			d.insert(QStringLiteral("nodes"), na);
			QJsonArray ua;
			for (const admin_directory_user_item &it : message.data.users)
				ua.append(admin_directory_user_item_to_json(it));
			d.insert(QStringLiteral("users"), ua);
			QJsonArray aa;
			for (const admin_directory_app_item &it : message.data.apps)
				aa.append(admin_directory_app_item_to_json(it));
			d.insert(QStringLiteral("apps"), aa);
			QJsonArray pa;
			for (const admin_directory_perm_item &it : message.data.permissions)
				pa.append(admin_directory_perm_item_to_json(it));
			d.insert(QStringLiteral("permissions"), pa);
			root.insert(QStringLiteral("data"), d);
		},
		[&](const envelope_file_list_req &message) {
			root.insert(QStringLiteral("type"), k_type_file_list_req());
			root.insert(QStringLiteral("token"), message.token);
			QJsonObject data_object;
			data_object.insert(QStringLiteral("path"), message.data.path);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_file_list_res &message) {
			root.insert(QStringLiteral("type"), k_type_file_list_res());
			QJsonObject data_object;
			data_object.insert(QStringLiteral("current_path"), message.data.current_path);
			QJsonArray entries_array;
			for (const file_entry &entry : message.data.entries) {
				entries_array.append(file_entry_to_json(entry));
			}
			data_object.insert(QStringLiteral("entries"), entries_array);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_file_download_init &message) {
			root.insert(QStringLiteral("type"), k_type_file_download_init());
			QJsonObject data_object;
			data_object.insert(QStringLiteral("path"), message.data.path);
			data_object.insert(QStringLiteral("chunk_size"), message.data.chunk_size);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_file_download_data &message) {
			root.insert(QStringLiteral("type"), k_type_file_download_data());
			QJsonObject data_object;
			data_object.insert(QStringLiteral("transfer_id"), message.data.transfer_id);
			data_object.insert(QStringLiteral("offset"), static_cast<double>(message.data.offset));
			data_object.insert(QStringLiteral("data"), message.data.data_base64);
			data_object.insert(QStringLiteral("is_eof"), message.data.is_eof);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_file_upload_chunk &message) {
			root.insert(QStringLiteral("type"), k_type_file_upload_chunk());
			QJsonObject data_object;
			data_object.insert(QStringLiteral("transfer_id"), message.data.transfer_id);
			data_object.insert(QStringLiteral("offset"), static_cast<double>(message.data.offset));
			data_object.insert(QStringLiteral("data"), message.data.data_base64);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_file_upload_ack &message) {
			root.insert(QStringLiteral("type"), k_type_file_upload_ack());
			root.insert(QStringLiteral("success"), message.success);
			QJsonObject data_object;
			data_object.insert(QStringLiteral("transfer_id"), message.data.transfer_id);
			data_object.insert(QStringLiteral("next_expected_offset"), static_cast<double>(message.data.next_expected_offset));
			data_object.insert(QStringLiteral("is_complete"), message.data.is_complete);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_control_action &message) {
			root.insert(QStringLiteral("type"), k_type_control_action());
			QJsonObject data_object;
			data_object.insert(QStringLiteral("action"), message.data.action);
			data_object.insert(QStringLiteral("force"), message.data.force);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_latency_ping &message) {
			root.insert(QStringLiteral("type"), k_type_latency_ping());
			QJsonObject data_object;
			data_object.insert(QStringLiteral("client_ts"), static_cast<double>(message.data.client_ts));
			data_object.insert(QStringLiteral("seq"), message.data.seq);
			root.insert(QStringLiteral("data"), data_object);
		},
		[&](const envelope_latency_pong &message) {
			root.insert(QStringLiteral("type"), k_type_latency_pong());
			QJsonObject data_object;
			data_object.insert(QStringLiteral("client_ts"), static_cast<double>(message.data.client_ts));
			data_object.insert(QStringLiteral("server_ts"), static_cast<double>(message.data.server_ts));
			data_object.insert(QStringLiteral("seq"), message.data.seq);
			root.insert(QStringLiteral("data"), data_object);
		},
	    },
	    message);

	const QJsonDocument document(root);
	return document.toJson(QJsonDocument::Compact);
}

QString external_api_protocol_facade::serialize_to_json_text(const external_api_message_variant &message) const
{
	const QByteArray utf8 = serialize_to_json_utf8(message);
	return QString::fromUtf8(utf8);
}
