/**
 * 信令 WebSocket 地址拼装：与 signaling_config.js +（仅 http/https）signaling_config.json 配合。
 *
 * 加载顺序：先 signaling_config.js，再 rpc_config.js；连接信令前可调用 __rpcEnsureSignalingJsonLoaded()。
 * file:// 下浏览器禁止读 JSON，只能改 signaling_config.js。
 *
 * 优先级：URL ?signaling= > JSON（http/https 合并后）> signaling.js > 内置默认
 */
(function (global) {
    'use strict';

    function mergeDefaults(overrides) {
        var base = {
            signalingBaseUrl: '',
            signalingHost: '127.0.0.1',
            signalingPort: 9090,
            /** 可选：本机 Receiver 服务健康检查 URL（如 http://127.0.0.1:17890/health），用于任务栏状态指示 */
            receiverHealthUrl: '',
        };
        if (!overrides || typeof overrides !== 'object') {
            return base;
        }
        if (overrides.signalingBaseUrl != null) {
            base.signalingBaseUrl = String(overrides.signalingBaseUrl).trim();
        }
        if (overrides.signalingHost != null) {
            base.signalingHost = String(overrides.signalingHost).trim();
        }
        if (overrides.signalingPort != null) {
            var p = parseInt(overrides.signalingPort, 10);
            if (!isNaN(p) && p > 0 && p < 65536) {
                base.signalingPort = p;
            }
        }
        if (overrides.receiverHealthUrl != null) {
            base.receiverHealthUrl = String(overrides.receiverHealthUrl).trim();
        }
        return base;
    }

    global.__rpcFrontendConfig = mergeDefaults(global.__rpcFrontendConfig);

    /** @returns {Promise<boolean>} 是否成功合并了 json（http/https 且文件可读） */
    global.__rpcEnsureSignalingJsonLoaded = function () {
        if (global.location.protocol === 'file:') {
            console.info(
                '[RemoteProcessControl] file:// 无法读取 signaling_config.json，信令地址以 signaling_config.js 为准'
            );
            return Promise.resolve(false);
        }
        if (global.location.protocol !== 'http:' && global.location.protocol !== 'https:') {
            return Promise.resolve(false);
        }
        var url;
        try {
            url = new URL('signaling_config.json', global.location.href).href;
        } catch (e) {
            return Promise.resolve(false);
        }
        return fetch(url, { cache: 'no-store' })
            .then(function (res) {
                if (!res.ok) {
                    throw new Error('HTTP ' + res.status);
                }
                return res.json();
            })
            .then(function (json) {
                if (!json || typeof json !== 'object') {
                    throw new Error('invalid json');
                }
                global.__rpcFrontendConfig = mergeDefaults(
                    Object.assign({}, global.__rpcFrontendConfig || {}, json)
                );
                console.info('[RemoteProcessControl] 已读取并合并 signaling_config.json', url, global.__rpcFrontendConfig);
                return true;
            })
            .catch(function (err) {
                console.info(
                    '[RemoteProcessControl] 未加载 signaling_config.json（将用 signaling_config.js）',
                    err && err.message ? err.message : err
                );
                return false;
            });
    };

    /**
     * @param {string} clientId
     * @returns {string}
     */
    global.__rpcBuildSignalingWebSocketUrl = function (clientId) {
        var params = new URLSearchParams(global.location.search);
        var signalingParam = params.get('signaling');
        if (signalingParam) {
            var base0 = signalingParam.replace(/\/+$/, '');
            return base0 + '/' + clientId;
        }
        var cfg = global.__rpcFrontendConfig || mergeDefaults(null);
        if (cfg.signalingBaseUrl) {
            var base1 = String(cfg.signalingBaseUrl).replace(/\/+$/, '');
            return base1 + '/' + clientId;
        }
        var isHttps = global.location.protocol === 'https:';
        var wsProto = isHttps ? 'wss:' : 'ws:';
        var host = cfg.signalingHost || global.location.hostname;
        if (!host && global.location.protocol === 'file:') {
            host = '127.0.0.1';
        }
        if (!host) {
            host = '127.0.0.1';
        }
        var port = cfg.signalingPort != null ? cfg.signalingPort : 9090;
        return wsProto + '//' + host + ':' + port + '/' + clientId;
    };
})(typeof window !== 'undefined' ? window : this);
