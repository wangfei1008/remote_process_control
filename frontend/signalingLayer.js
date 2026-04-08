/**
 * 信令层
 */
(function () {
    'use strict';

    window.__rpcSignaling = window.__rpcSignaling || {};

    function buildSignalingWebSocketUrl(clientId) {
        const params = new URLSearchParams(window.location.search);
        const signaling = params.get('signaling');
        if (signaling) {
            const base = signaling.replace(/\/+$/, '');
            return base + '/' + clientId;
        }
        const isHttps = window.location.protocol === 'https:';
        const wsProto = isHttps ? 'wss:' : 'ws:';
        let host = window.location.hostname;
        if (!host && window.location.protocol === 'file:') host = '127.0.0.1';
        if (!host) host = '127.0.0.1';
        return wsProto + '//' + host + ':9090/' + clientId;
    }

    window.__rpcSignaling.createSignalingClient = function createSignalingClient(session, doc, ui, callbacks) {
        const onOffer = callbacks.onOffer;
        const internal = window.__rpcInternal || {};
        const updateWebSocketState = internal.updateWebSocketState;
        const clearSessionTimer = internal.clearSessionTimer;
        const logDataChannel = internal.logDataChannel;
        const shouldDeferRpcShellCloseUntilVideo = internal.shouldDeferRpcShellCloseUntilVideo;
        const closeRpcShellOrWindow = internal.closeRpcShellOrWindow;

        function connect() {
            try {
                const wsUrl = buildSignalingWebSocketUrl(session.clientId);
                console.info('[RemoteProcessControl] 信令 WebSocket:', wsUrl);
                session.websocket = new WebSocket(wsUrl);
                if (updateWebSocketState) updateWebSocketState(ui, 'connecting');

                session.websocket.onopen = function () {
                    if (clearSessionTimer) clearSessionTimer(session, 'rpcWsConnectTimer');
                    if (updateWebSocketState) updateWebSocketState(ui, 'connected');
                    const startBtn = doc.getElementById('start');
                    if (startBtn) startBtn.disabled = false;
                    if (logDataChannel) logDataChannel(ui, 'WebSocket connected. clientId=' + session.clientId);
                    if (typeof callbacks.onWebSocketOpen === 'function') {
                        try {
                            callbacks.onWebSocketOpen();
                        } catch (cbErr) {
                            console.error('[RemoteProcessControl] onWebSocketOpen:', cbErr);
                        }
                    }
                };

                session.websocket.onmessage = function (evt) {
                    if (typeof evt.data !== 'string') return;
                    let message;
                    try {
                        message = JSON.parse(evt.data);
                    } catch (parseErr) {
                        console.warn('[RemoteProcessControl] 非 JSON 信令消息，已忽略:', evt.data, parseErr);
                        return;
                    }
                    if (message.type === 'offer') {
                        const offerEl = doc.getElementById('offer-sdp');
                        if (offerEl) offerEl.textContent = message.sdp;
                        Promise.resolve(onOffer(message)).catch(function (offerErr) {
                            console.error('[RemoteProcessControl] 处理 offer 失败:', offerErr);
                            if (logDataChannel) {
                                logDataChannel(ui, '处理 offer 失败: ' + (offerErr && offerErr.message ? offerErr.message : offerErr));
                            }
                        });
                    }
                };

                session.websocket.onerror = function () {
                    if (logDataChannel) {
                        logDataChannel(ui, 'WebSocket 错误（请确认信令服务已监听 9090，且 HTTPS 页面需使用 WSS 或改为 HTTP 打开前端）');
                    }
                    if (updateWebSocketState) updateWebSocketState(ui, 'error');
                    if (session.rpcWindowMode || session.electronCompactLauncher) {
                        window.setTimeout(function () {
                            if (!shouldDeferRpcShellCloseUntilVideo || !shouldDeferRpcShellCloseUntilVideo(session)) {
                                if (closeRpcShellOrWindow) closeRpcShellOrWindow(session, 'websocket_error');
                            }
                        }, 500);
                    }
                };

                session.websocket.onclose = function (ev) {
                    if (logDataChannel) {
                        logDataChannel(ui, 'WebSocket closed (code=' + ev.code + (ev.reason ? ', ' + ev.reason : '') + ')');
                    }
                    if (updateWebSocketState) updateWebSocketState(ui, 'disconnected');
                    if (session.rpcWindowMode || session.electronCompactLauncher) {
                        window.setTimeout(function () {
                            if (!shouldDeferRpcShellCloseUntilVideo || !shouldDeferRpcShellCloseUntilVideo(session)) {
                                if (closeRpcShellOrWindow) closeRpcShellOrWindow(session, 'websocket_closed_' + ev.code);
                            }
                        }, 400);
                    }
                };
            } catch (e) {
                if (logDataChannel) logDataChannel(ui, 'Failed to init WebSocket: ' + e);
                if (updateWebSocketState) updateWebSocketState(ui, 'error');
                console.error('[RemoteProcessControl] WebSocket 构造失败:', e);
            }
        }

        return { connect: connect };
    };
})();

