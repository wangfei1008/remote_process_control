/**
 * 可选：探测本机 Receiver 服务是否可达（低延时解码链路占位）
 * 在 signaling_config.json / signaling_config.js 中配置 receiverHealthUrl，例如 http://127.0.0.1:17890/health
 */
(function (global) {
    'use strict';

    global.__rpcReceiverServiceOnline = null;

    global.__rpcProbeReceiverService = function () {
        var cfg = global.__rpcFrontendConfig || {};
        var url = String(cfg.receiverHealthUrl || '').trim();
        if (!url) {
            global.__rpcReceiverServiceOnline = null;
            try {
                global.dispatchEvent(
                    new CustomEvent('rpc-receiver-probe', { detail: { configured: false } })
                );
            } catch (_) {}
            return;
        }
        global.fetch(url, { method: 'GET', mode: 'cors', cache: 'no-store' })
            .then(function (r) {
                var ok = !!(r && r.ok);
                global.__rpcReceiverServiceOnline = ok;
                try {
                    global.dispatchEvent(
                        new CustomEvent('rpc-receiver-probe', { detail: { configured: true, online: ok } })
                    );
                } catch (_) {}
            })
            .catch(function () {
                global.__rpcReceiverServiceOnline = false;
                try {
                    global.dispatchEvent(
                        new CustomEvent('rpc-receiver-probe', {
                            detail: { configured: true, online: false },
                        })
                    );
                } catch (_) {}
            });
    };
})(typeof window !== 'undefined' ? window : this);
