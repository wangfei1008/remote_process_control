/**
 * 与 signaling_server（external_api）交互：登录、授权应用列表、图标。
 */
(function (global) {
    'use strict';

    function randomAuthClientId() {
        try {
            if (typeof global.__rpcRandomId === 'function') {
                return 'auth_' + global.__rpcRandomId(14);
            }
        } catch (_) {}
        return 'auth_' + String(Date.now()) + '_' + Math.floor(Math.random() * 1e9);
    }

    function openWebSocket(url, timeoutMs) {
        return new Promise(function (resolve, reject) {
            var ws;
            try {
                ws = new WebSocket(url);
            } catch (e) {
                reject(e);
                return;
            }
            var t = setTimeout(function () {
                try {
                    ws.close();
                } catch (_) {}
                reject(new Error('WebSocket open timeout'));
            }, timeoutMs || 12000);
            ws.onopen = function () {
                clearTimeout(t);
                resolve(ws);
            };
            ws.onerror = function () {
                clearTimeout(t);
                reject(new Error('WebSocket error'));
            };
        });
    }

    /** 登录口令：前端 MD5（UTF-8）十六进制小写，服务端再 SHA-256 与库中比对 */
    function passwordForLoginTransport(plain) {
        if (typeof global.__rpcMd5Hex === 'function') {
            return global.__rpcMd5Hex(plain);
        }
        try {
            console.warn('[rpc-auth] rpc_md5.js 未加载，口令将明文发送（仅用于排障）');
        } catch (_) {}
        return String(plain || '');
    }

    function waitNextJson(ws, predicate, timeoutMs) {
        return new Promise(function (resolve, reject) {
            var done = false;
            var t = setTimeout(function () {
                if (done) return;
                done = true;
                try {
                    ws.close();
                } catch (_) {}
                reject(new Error('signaling RPC timeout'));
            }, timeoutMs || 15000);

            function onMsg(ev) {
                if (done) return;
                if (typeof ev.data !== 'string') return;
                var obj;
                try {
                    obj = JSON.parse(ev.data);
                } catch (_) {
                    return;
                }
                try {
                    if (predicate(obj)) {
                        done = true;
                        clearTimeout(t);
                        ws.removeEventListener('message', onMsg);
                        resolve(obj);
                    }
                } catch (_) {}
            }
            ws.addEventListener('message', onMsg);
        });
    }

    /**
     * @returns {Promise<{token: string, role: string, apps: Array, iconsByAppId: Object}>}
     */
    function loginAndFetchCatalog(username, password) {
        if (!global.__rpcBuildSignalingWebSocketUrl) {
            return Promise.reject(new Error('rpc_config 未加载'));
        }
        var url = global.__rpcBuildSignalingWebSocketUrl(randomAuthClientId());
        var wsRef = { ws: null };

        return openWebSocket(url, 12000)
            .then(function (ws) {
                wsRef.ws = ws;
                ws.send(
                    JSON.stringify({
                        type: 'login',
                        data: {
                            username: String(username || ''),
                            password: passwordForLoginTransport(password),
                        },
                    })
                );
                return waitNextJson(
                    ws,
                    function (o) {
                        return o && o.type === 'login_res';
                    },
                    12000
                );
            })
            .then(function (loginRes) {
                if (!loginRes || !loginRes.success) {
                    try {
                        if (wsRef.ws) wsRef.ws.close();
                    } catch (_) {}
                    return Promise.reject(new Error('用户名或密码错误'));
                }
                var token =
                    loginRes.data && loginRes.data.token ? String(loginRes.data.token) : '';
                var role = loginRes.data && loginRes.data.role ? String(loginRes.data.role) : '';
                if (!token) {
                    try {
                        if (wsRef.ws) wsRef.ws.close();
                    } catch (_) {}
                    return Promise.reject(new Error('未返回 token'));
                }
                if (!wsRef.ws || wsRef.ws.readyState !== WebSocket.OPEN) {
                    return Promise.reject(new Error('信令连接已断开'));
                }
                wsRef.ws.send(JSON.stringify({ type: 'get_auth_apps', token: token }));
                return waitNextJson(
                    wsRef.ws,
                    function (o) {
                        return o && o.type === 'auth_apps_res';
                    },
                    12000
                ).then(function (appsRes) {
                    return { token: token, role: role, appsRes: appsRes };
                });
            })
            .then(function (ctx) {
                var apps = Array.isArray(ctx.appsRes.apps) ? ctx.appsRes.apps : [];
                var ids = [];
                for (var i = 0; i < apps.length; i++) {
                    if (apps[i] && apps[i].app_id != null) ids.push(Number(apps[i].app_id));
                }
                if (!ids.length) {
                    try {
                        if (wsRef.ws) wsRef.ws.close();
                    } catch (_) {}
                    return {
                        token: ctx.token,
                        role: ctx.role,
                        apps: apps,
                        iconsByAppId: {},
                    };
                }
                if (!wsRef.ws || wsRef.ws.readyState !== WebSocket.OPEN) {
                    return {
                        token: ctx.token,
                        role: ctx.role,
                        apps: apps,
                        iconsByAppId: {},
                    };
                }
                wsRef.ws.send(
                    JSON.stringify({
                        type: 'get_apps_icons',
                        token: ctx.token,
                        app_ids: ids,
                    })
                );
                return waitNextJson(
                    wsRef.ws,
                    function (o) {
                        return o && o.type === 'apps_icons_res';
                    },
                    12000
                ).then(function (iconsRes) {
                    try {
                        if (wsRef.ws) wsRef.ws.close();
                    } catch (_) {}
                    var map = {};
                    var icons = Array.isArray(iconsRes.icons) ? iconsRes.icons : [];
                    for (var j = 0; j < icons.length; j++) {
                        var it = icons[j];
                        if (!it || it.app_id == null) continue;
                        var b64 = String(it.icon_base64 || '').trim();
                        if (!b64) continue;
                        if (/^https?:\/\//i.test(b64) || /^data:/i.test(b64)) {
                            map[String(it.app_id)] = b64;
                        } else {
                            map[String(it.app_id)] = 'data:image/png;base64,' + b64;
                        }
                    }
                    return {
                        token: ctx.token,
                        role: ctx.role,
                        apps: apps,
                        iconsByAppId: map,
                    };
                });
            });
    }

    /**
     * 使用已有 token 刷新授权应用与图标（会话恢复）
     * @returns {Promise<{apps: Array, iconsByAppId: Object}>}
     */
    function fetchCatalogWithToken(token) {
        if (!global.__rpcBuildSignalingWebSocketUrl) {
            return Promise.reject(new Error('rpc_config 未加载'));
        }
        if (!token) return Promise.reject(new Error('无 token'));
        var url = global.__rpcBuildSignalingWebSocketUrl(randomAuthClientId());
        var wsRef = { ws: null };
        return openWebSocket(url, 12000)
            .then(function (ws) {
                wsRef.ws = ws;
                ws.send(JSON.stringify({ type: 'get_auth_apps', token: String(token) }));
                return waitNextJson(
                    ws,
                    function (o) {
                        return o && o.type === 'auth_apps_res';
                    },
                    12000
                );
            })
            .then(function (appsRes) {
                var apps = Array.isArray(appsRes.apps) ? appsRes.apps : [];
                var ids = [];
                for (var i = 0; i < apps.length; i++) {
                    if (apps[i] && apps[i].app_id != null) ids.push(Number(apps[i].app_id));
                }
                if (!ids.length) {
                    try {
                        if (wsRef.ws) wsRef.ws.close();
                    } catch (_) {}
                    return { apps: apps, iconsByAppId: {} };
                }
                if (!wsRef.ws || wsRef.ws.readyState !== WebSocket.OPEN) {
                    return { apps: apps, iconsByAppId: {} };
                }
                wsRef.ws.send(
                    JSON.stringify({
                        type: 'get_apps_icons',
                        token: String(token),
                        app_ids: ids,
                    })
                );
                return waitNextJson(
                    wsRef.ws,
                    function (o) {
                        return o && o.type === 'apps_icons_res';
                    },
                    12000
                ).then(function (iconsRes) {
                    try {
                        if (wsRef.ws) wsRef.ws.close();
                    } catch (_) {}
                    var map = {};
                    var icons = Array.isArray(iconsRes.icons) ? iconsRes.icons : [];
                    for (var j = 0; j < icons.length; j++) {
                        var it = icons[j];
                        if (!it || it.app_id == null) continue;
                        var b64 = String(it.icon_base64 || '').trim();
                        if (!b64) continue;
                        if (/^https?:\/\//i.test(b64) || /^data:/i.test(b64)) {
                            map[String(it.app_id)] = b64;
                        } else {
                            map[String(it.app_id)] = 'data:image/png;base64,' + b64;
                        }
                    }
                    return { apps: apps, iconsByAppId: map };
                });
            });
    }

    /**
     * 管理员统计（需 admin 角色 token）
     */
    function fetchAdminStats(token) {
        if (!global.__rpcBuildSignalingWebSocketUrl) {
            return Promise.reject(new Error('rpc_config 未加载'));
        }
        if (!token) return Promise.reject(new Error('无 token'));
        var url = global.__rpcBuildSignalingWebSocketUrl(randomAuthClientId());
        return openWebSocket(url, 12000)
            .then(function (ws) {
                ws.send(JSON.stringify({ type: 'get_admin_stats', token: String(token) }));
                return waitNextJson(
                    ws,
                    function (o) {
                        return o && o.type === 'admin_stats_res';
                    },
                    18000
                ).then(function (res) {
                    try {
                        ws.close();
                    } catch (_) {}
                    return res;
                });
            })
            .then(function (res) {
                var d = (res && res.data) || {};
                return {
                    active_sessions:
                        typeof d.active_sessions === 'number' ? d.active_sessions : parseInt(d.active_sessions, 10) || 0,
                    nodes_online: Array.isArray(d.nodes_online) ? d.nodes_online : [],
                    usage_ranking: Array.isArray(d.usage_ranking) ? d.usage_ranking : [],
                };
            });
    }

    /**
     * 管理员全量目录：节点、用户、应用、权限（管控台）
     */
    function fetchAdminDirectory(token) {
        if (!global.__rpcBuildSignalingWebSocketUrl) {
            return Promise.reject(new Error('rpc_config 未加载'));
        }
        if (!token) return Promise.reject(new Error('无 token'));
        var url = global.__rpcBuildSignalingWebSocketUrl(randomAuthClientId());
        return openWebSocket(url, 12000)
            .then(function (ws) {
                ws.send(JSON.stringify({ type: 'get_admin_directory', token: String(token) }));
                return waitNextJson(
                    ws,
                    function (o) {
                        return o && o.type === 'admin_directory_res';
                    },
                    18000
                ).then(function (res) {
                    try {
                        ws.close();
                    } catch (_) {}
                    return res;
                });
            })
            .then(function (res) {
                var d = (res && res.data) || {};
                return {
                    nodes: Array.isArray(d.nodes) ? d.nodes : [],
                    users: Array.isArray(d.users) ? d.users : [],
                    apps: Array.isArray(d.apps) ? d.apps : [],
                    permissions: Array.isArray(d.permissions) ? d.permissions : [],
                };
            });
    }

    /**
     * 管控台统一变更：node / application / user / permission 的 create、update、delete
     * @param {string} token
     * @param {{ entity: string, action: string, payload: object }} body
     */
    function adminMutate(token, body) {
        if (!global.__rpcBuildSignalingWebSocketUrl) {
            return Promise.reject(new Error('rpc_config 未加载'));
        }
        if (!token) return Promise.reject(new Error('无 token'));
        var b = body || {};
        var url = global.__rpcBuildSignalingWebSocketUrl(randomAuthClientId());
        return openWebSocket(url, 12000)
            .then(function (ws) {
                ws.send(
                    JSON.stringify({
                        type: 'admin_mutate',
                        token: String(token),
                        data: {
                            entity: String(b.entity || ''),
                            action: String(b.action || ''),
                            payload: b.payload && typeof b.payload === 'object' ? b.payload : {},
                        },
                    })
                );
                return waitNextJson(
                    ws,
                    function (o) {
                        return o && o.type === 'admin_mutate_res';
                    },
                    20000
                ).then(function (res) {
                    try {
                        ws.close();
                    } catch (_) {}
                    return res;
                });
            })
            .then(function (res) {
                if (!res || !res.success) {
                    var msg = (res && res.message) || '操作失败';
                    return Promise.reject(new Error(msg));
                }
                var d = (res && res.data) || {};
                return { success: true, entity: d.entity, id: d.id != null ? d.id : 0, raw: res };
            });
    }

    /**
     * 登记远程应用（需 admin token）
     */
    function adminAddApp(token, payload) {
        if (!global.__rpcBuildSignalingWebSocketUrl) {
            return Promise.reject(new Error('rpc_config 未加载'));
        }
        if (!token) return Promise.reject(new Error('无 token'));
        var p = payload || {};
        var url = global.__rpcBuildSignalingWebSocketUrl(randomAuthClientId());
        return openWebSocket(url, 12000)
            .then(function (ws) {
                ws.send(
                    JSON.stringify({
                        type: 'admin_add_app',
                        token: String(token),
                        data: {
                            node_name: String(p.node_name || '').trim(),
                            display_name: String(p.display_name || '').trim(),
                            exe_path: String(p.exe_path || '').trim(),
                            icon_base64: String(p.icon_base64 || '').trim(),
                            is_public: p.is_public ? 1 : 0,
                        },
                    })
                );
                return waitNextJson(
                    ws,
                    function (o) {
                        return o && o.type === 'admin_add_app' && Object.prototype.hasOwnProperty.call(o, 'success');
                    },
                    18000
                ).then(function (res) {
                    try {
                        ws.close();
                    } catch (_) {}
                    return res;
                });
            })
            .then(function (res) {
                if (!res || !res.success) {
                    return Promise.reject(
                        new Error('登记失败（请确认采集节点名称已在库中、且当前为管理员账号）')
                    );
                }
                var appId =
                    res.data && res.data.app_id != null ? res.data.app_id : null;
                return { success: true, app_id: appId, raw: res };
            });
    }

    global.__rpcAuthSignaling = {
        loginAndFetchCatalog: loginAndFetchCatalog,
        fetchCatalogWithToken: fetchCatalogWithToken,
        fetchAdminStats: fetchAdminStats,
        fetchAdminDirectory: fetchAdminDirectory,
        adminMutate: adminMutate,
        adminAddApp: adminAddApp,
    };
})(typeof window !== 'undefined' ? window : this);
