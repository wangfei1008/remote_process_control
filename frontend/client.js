/**
 * 主入口：兼容 file:// 双击打开（非 ES Module）。
 */
(function () {
    'use strict';

    function getTargetWorkNodeId() {
        try {
            var qs = new URLSearchParams(window.location.search);
            var v = String(qs.get('workNode') || '').trim();
            // workNode 约束：1–30 字符；不满足则回退 server
            if (v && v.length >= 1 && v.length <= 30) return v;
        } catch (_) {}
        return 'server';
    }

    function createSession() {
        function getOrCreateStableClientId() {
            // Keep the same clientId across reloads to enable reconnect/resume on server.
            // Can be overridden by URL ?clientId=xxx
            try {
                var qs = new URLSearchParams(window.location.search);
                var forced = String(qs.get('clientId') || '').trim();
                if (forced) return forced;
            } catch (_) {}
            try {
                var key = 'rpc_client_id_v1';
                var existing = String(window.localStorage.getItem(key) || '').trim();
                if (existing) return existing;
                var v = window.__rpcRandomId(10);
                window.localStorage.setItem(key, v);
                return v;
            } catch (_) {}
            return window.__rpcRandomId(10);
        }
        return {
            clientId: getOrCreateStableClientId(),
            websocket: null,
            pc: null,
            dc: null,
            activeVideo: null,
            rpcStreamW: 1536,
            rpcStreamH: 864,
            remoteControlEnabled: true,
            isControlEnabled: false,
            /** rpcWindow mode: pass exePath via URL to auto-start the remote app. */
            rpcExePath: null,
            /** 目标工作节点（用于信令路由 id，替代硬编码 "server"） */
            rpcWorkNode: 'server',
            /** 独立窗口/应用模式：隐藏网页控制台，仅全屏远程画面（由 URL ?rpcWindow=1 开启） */
            rpcWindowMode: false,
            /** WebSocket 连通后自动 Start（应用模式默认 true，可用 autostart=0 关闭） */
            rpcAutostart: false,
            rpcHadVideo: false,
            rpcAutoClosed: false,
            /** 视频页：默认 60 秒无视频则主动退出（可用 URL ?rpcVideoTimeoutMs= 覆盖） */
            rpcVideoTimeoutMs: 60000,
            /** 应用模式：WebSocket 多久未连上则关窗（ms），URL ?rpcWsConnectTimeoutMs= */
            rpcWsConnectTimeoutMs: 45000,
            rpcNoVideoTimer: null,
            rpcWsConnectTimer: null,
            rpcPcDisconnectTimer: null,
            rpcVideoPageOpenedAt: 0,
            rpcVideoLastAliveAt: 0,
            /** 远端进程结束/视频轨已断后已执行退出画面，避免重复 stop */
            rpcRemoteStreamExitHandled: false,
        };
    }

    function rpcShellCloseAllowed(session) {
        return session.rpcWindowMode;
    }

    function clearSessionTimer(session, key) {
        const id = session[key];
        if (id != null) {
            clearTimeout(id);
            clearInterval(id);
            session[key] = null;
        }
    }

    function nowTs() {
        try { return new Date().toISOString(); } catch (_) { return String(Date.now()); }
    }

    function wsStateName(ws) {
        try {
            if (!ws) return 'null';
            const v = ws.readyState;
            if (v === WebSocket.CONNECTING) return 'CONNECTING';
            if (v === WebSocket.OPEN) return 'OPEN';
            if (v === WebSocket.CLOSING) return 'CLOSING';
            if (v === WebSocket.CLOSED) return 'CLOSED';
            return String(v);
        } catch (_) { return 'unknown'; }
    }

    function pcStateSnapshot(pc) {
        try {
            if (!pc) return { conn: 'null', ice: 'null', signaling: 'null' };
            return {
                conn: String(pc.connectionState || ''),
                ice: String(pc.iceConnectionState || ''),
                signaling: String(pc.signalingState || ''),
            };
        } catch (_) { return { conn: 'err', ice: 'err', signaling: 'err' }; }
    }

    function recordVideoPageExitReason(session, reason, extra) {
        if (!session) return;
        const r = String(reason || '').trim() || 'unknown';
        const x = extra || {};
        try {
            session.__rpc_exit_reason = r;
            session.__rpc_exit_ts = nowTs();
            session.__rpc_exit_extra = x;
        } catch (_) {}
        try {
            const pc = pcStateSnapshot(session.pc);
            console.warn('[rpc-exit] ts=' + nowTs()
                + ' clientId=' + session.clientId
                + ' reason=' + r
                + ' ws=' + wsStateName(session.websocket)
                + ' pc.conn=' + pc.conn
                + ' pc.ice=' + pc.ice
                + ' pc.sig=' + pc.signaling
                + ' hadVideo=' + (session.rpcHadVideo ? 1 : 0)
                + ' remoteExitHandled=' + (session.rpcRemoteStreamExitHandled ? 1 : 0)
                + ' autoClosed=' + (session.rpcAutoClosed ? 1 : 0)
                + (x && x.detail ? (' detail=' + String(x.detail)) : ''));
        } catch (_) {}
    }

    /**
     * Electron 紧凑启动（双击磁贴后再连）：未收到首帧前不因信令断开/ICE 失败等关窗，
     * 仅依赖「无视频超时」；出画后恢复对断流、关 track 等的自动关窗。
     */
    function shouldDeferRpcShellCloseUntilVideo(session) {
        void session;
        return false;
    }

    /** 应用模式下自动关窗（Electron 关进程；浏览器尝试 window.close） */
    function closeRpcShellOrWindow(session, reason) {
        if (!rpcShellCloseAllowed(session) || session.rpcAutoClosed) return;
        recordVideoPageExitReason(session, reason || 'rpc_window_close', { detail: 'closeRpcShellOrWindow' });
        session.rpcAutoClosed = true;
        clearSessionTimer(session, 'rpcNoVideoTimer');
        clearSessionTimer(session, 'rpcWsConnectTimer');
        clearSessionTimer(session, 'rpcPcDisconnectTimer');
        console.warn('[rpc-window] 自动关闭窗口: ' + (reason || ''));
        try {
            if (window.parent && window.parent !== window && typeof window.parent.postMessage === 'function') {
                window.parent.postMessage({
                    type: 'rpc_request_close',
                    reason: reason || '',
                }, '*');
                return;
            }
        } catch (_) {}
        if (window.rpcShell && typeof window.rpcShell.close === 'function') {
            try {
                window.rpcShell.close();
            } catch (_) {}
            return;
        }
        try {
            window.close();
        } catch (_) {}
    }

    /**
     * 远端已无视频流（进程退出、track ended）：应用/Electron 模式关窗；普通页面则 stop() 退出视频层并断开 WebRTC。
     */
    function exitVideoPageAfterRemoteStreamEnded(session, doc, ui, reason) {
        if (session.rpcRemoteStreamExitHandled || session.rpcAutoClosed) return;
        session.rpcRemoteStreamExitHandled = true;
        recordVideoPageExitReason(session, reason || 'remote_stream_ended', { detail: 'exitVideoPageAfterRemoteStreamEnded' });
        if (rpcShellCloseAllowed(session)) {
            closeRpcShellOrWindow(session, reason);
            return;
        }
        const app = window.__rpcApp;
        if (app && typeof app.stop === 'function') {
            try {
                app.stop();
            } catch (err) {
                console.warn('[RemoteProcessControl] 远端断流后 stop 失败:', err);
                hideVideoStage(doc);
            }
        } else {
            hideVideoStage(doc);
            const v = getMainVideo(doc);
            if (v) {
                v.__rpc_keyboard_bound = false;
                v.srcObject = null;
            }
            const stg = getVideoStage(doc);
            if (stg) stg.__rpc_stage_mouse_bound = false;
            session.activeVideo = null;
            session.rpcHadVideo = false;
            if (ui) logDataChannel(ui, '远端已无视频流，已退出画面（' + reason + '）');
        }
    }

    function forceExitVideoPageOnNoVideoTimeout(session, doc, ui, reason) {
        recordVideoPageExitReason(session, reason || 'no_video_timeout', { detail: 'forceExitVideoPageOnNoVideoTimeout' });
        if (rpcShellCloseAllowed(session)) {
            closeRpcShellOrWindow(session, reason);
            return;
        }
        const app = window.__rpcApp;
        if (app && typeof app.stop === 'function') {
            try {
                app.stop();
            } catch (_) {
                hideVideoStage(doc);
            }
        } else {
            hideVideoStage(doc);
            const v = getMainVideo(doc);
            if (v) v.srcObject = null;
        }
        if (ui) logDataChannel(ui, '无视频流超时，已退出画面（' + reason + '）');
    }

    function armRpcNoVideoWatchdog(session, doc, ui) {
        if (session.rpcAutoClosed) return;
        clearSessionTimer(session, 'rpcNoVideoTimer');
        const ms = Math.max(3000, Number(session.rpcVideoTimeoutMs) || 60000);
        session.rpcVideoPageOpenedAt = Date.now();
        session.rpcNoVideoTimer = setInterval(function () {
            if (session.rpcAutoClosed) return;
            const stage = getVideoStage(doc);
            if (!stage || stage.hidden) return;
            const baseTs = session.rpcVideoLastAliveAt || session.rpcVideoPageOpenedAt;
            if (!baseTs) return;
            if ((Date.now() - baseTs) < ms) return;
            clearSessionTimer(session, 'rpcNoVideoTimer');
            forceExitVideoPageOnNoVideoTimeout(session, doc, ui, 'timeout_no_video_' + ms + 'ms');
        }, 1000);
    }

    function clearRpcNoVideoWatchdog(session) {
        clearSessionTimer(session, 'rpcNoVideoTimer');
    }

    function onRpcVideoStreamReady(session) {
        session.rpcHadVideo = true;
        session.rpcVideoLastAliveAt = Date.now();
        clearSessionTimer(session, 'rpcWsConnectTimer');
    }

    function armRpcWebSocketConnectWatchdog(session) {
        if (!session.rpcWindowMode || session.rpcAutoClosed) return;
        clearSessionTimer(session, 'rpcWsConnectTimer');
        const wsMs = Math.max(3000, Number(session.rpcWsConnectTimeoutMs) || 45000);
        session.rpcWsConnectTimer = setTimeout(function () {
            session.rpcWsConnectTimer = null;
            if (session.rpcHadVideo) return;
            if (session.websocket && session.websocket.readyState === WebSocket.OPEN) return;
            closeRpcShellOrWindow(session, 'websocket_connect_timeout_' + wsMs + 'ms');
        }, wsMs);
    }

    /**
     * rpcWindow=1 或 kiosk=1：像「本地程序」一样只显示远程窗口。
     * autostart：显式 1/0；未指定时，在 rpcWindow 下默认自动连接。
     */
    function applyRpcWindowUrlFlags(session, doc) {
        const params = new URLSearchParams(window.location.search);
        const rpcWindow = params.get('rpcWindow') === '1' || params.get('kiosk') === '1';
        const autostartParam = params.get('autostart');
        let rpcAutostart = false;
        if (autostartParam === '0') {
            rpcAutostart = false;
        } else if (autostartParam === '1') {
            rpcAutostart = true;
        } else {
            rpcAutostart = rpcWindow;
        }
        session.rpcWindowMode = rpcWindow;
        session.rpcAutostart = rpcAutostart;
        session.rpcWorkNode = getTargetWorkNodeId();
        if (rpcWindow) {
            const tmo = parseInt(params.get('rpcVideoTimeoutMs') || '60000', 10);
            session.rpcVideoTimeoutMs = !isNaN(tmo) && tmo >= 3000 ? tmo : 60000;

            // Pass app exe path from URL for multi-window desktop container.
            const exePathParam = params.get('exePath');
            if (exePathParam && String(exePathParam).trim() !== '') {
                session.rpcExePath = exePathParam;
            }
        }

        const wTitle = params.get('windowTitle');
        if (wTitle) {
            try {
                doc.title = decodeURIComponent(wTitle);
            } catch (_) {
                doc.title = wTitle;
            }
        } else if (rpcWindow) {
            doc.title = '远程应用';
        }

        if (!rpcWindow) return;

        doc.documentElement.classList.add('rpc-window-mode');
        const stage = doc.getElementById('video-stage');
        if (stage) stage.hidden = false;
        const splash = doc.getElementById('rpc-window-wait');
        if (splash) splash.hidden = false;
    }

    function bindStatusElements(doc) {
        // 经典单页 UI 已移除：保留一个最小对象，避免调用方空指针。
        void doc;
        return {};
    }

    function logDataChannel(ui, message) {
        void ui;
        console.info('[rpc]', message);
    }

    function updateWebSocketState(ui, state) {
        void ui;
        console.info('[rpc] websocket:', state);
    }

    function updateDataChannelState(ui, state) {
        void ui;
        console.info('[rpc] datachannel:', state);
    }

    function getMainVideo(doc) {
        if (window.__rpcUI && typeof window.__rpcUI.getMainVideo === 'function') {
            return window.__rpcUI.getMainVideo(doc);
        }
        return doc.getElementById('rpc-main-video');
    }

    function getVideoStage(doc) {
        if (window.__rpcUI && typeof window.__rpcUI.getVideoStage === 'function') {
            return window.__rpcUI.getVideoStage(doc);
        }
        return doc.getElementById('video-stage');
    }

    function applyVideoDisplaySize(video) {
        if (window.__rpcUI && typeof window.__rpcUI.applyVideoDisplaySize === 'function') {
            window.__rpcUI.applyVideoDisplaySize(video);
        }
    }

    function tryEnterStageFullscreen(doc) {
        if (window.__rpcUI && typeof window.__rpcUI.tryEnterStageFullscreen === 'function') {
            window.__rpcUI.tryEnterStageFullscreen(doc);
        }
    }

    function exitDocumentFullscreen(doc) {
        if (window.__rpcUI && typeof window.__rpcUI.exitDocumentFullscreen === 'function') {
            window.__rpcUI.exitDocumentFullscreen(doc);
        }
    }

    function showVideoStage(session, doc) {
        if (window.__rpcUI && typeof window.__rpcUI.showVideoStage === 'function') {
            window.__rpcUI.showVideoStage(session, doc);
        }
    }

    function hideVideoStage(doc) {
        if (window.__rpcUI && typeof window.__rpcUI.hideVideoStage === 'function') {
            window.__rpcUI.hideVideoStage(doc);
        }
    }

    function updateVideoSizeInfo(doc) {
        if (window.__rpcUI && typeof window.__rpcUI.updateVideoSizeInfo === 'function') {
            window.__rpcUI.updateVideoSizeInfo(doc);
        }
    }

    function canSendControl(session) {
        if (window.__rpcUI && typeof window.__rpcUI.canSendControl === 'function') {
            return window.__rpcUI.canSendControl(session);
        }
        return session.remoteControlEnabled && session.dc && session.dc.readyState === 'open';
    }

    function isEventOnVideoHud(target) {
        if (window.__rpcUI && typeof window.__rpcUI.isEventOnVideoHud === 'function') {
            return window.__rpcUI.isEventOnVideoHud(target);
        }
        return target && target.closest && target.closest('.video-stage-hud');
    }

    function sendMouseMoveFromEvent(session, video, event) {
        if (window.__rpcUI && typeof window.__rpcUI.sendMouseMoveFromEvent === 'function') {
            window.__rpcUI.sendMouseMoveFromEvent(session, video, event);
        }
    }

    function setupStageMouse(session, doc) {
        if (window.__rpcUI && typeof window.__rpcUI.setupStageMouse === 'function') {
            window.__rpcUI.setupStageMouse(session, doc);
        }
    }

    function setupKeyboardOnStage(session, doc) {
        if (window.__rpcUI && typeof window.__rpcUI.setupKeyboardOnStage === 'function') {
            window.__rpcUI.setupKeyboardOnStage(session, doc);
        }
    }

    function setupRemoteControl(session, doc) {
        if (window.__rpcUI && typeof window.__rpcUI.setupRemoteControl === 'function') {
            window.__rpcUI.setupRemoteControl(session, doc);
        }
    }

    function createSignalingClient(session, doc, ui, callbacks) {
        if (window.__rpcSignaling && typeof window.__rpcSignaling.createSignalingClient === 'function') {
            return window.__rpcSignaling.createSignalingClient(session, doc, ui, callbacks);
        }
        console.error('[RemoteProcessControl] signalingLayer 未加载，无法创建信令客户端');
        return { connect: function () {} };
    }

    function RemoteProcessApplication(doc) {
        this.doc = doc;
        this.session = createSession();
        this.ui = null;
        this.webrtc = null;
        this.signaling = null;
    }

    /** 供 webrtcLayer / signalingLayer 使用的内部回调（session 生命周期、UI 日志等） */
    function bindLayerExports() {
        try {
            window.__rpcInternal = window.__rpcInternal || {};
            const i = window.__rpcInternal;
            i.createSignalingClient = createSignalingClient;
            i.showVideoStage = showVideoStage;
            i.hideVideoStage = hideVideoStage;
            i.setupRemoteControl = setupRemoteControl;
            i.setupStageMouse = setupStageMouse;
            i.setupKeyboardOnStage = setupKeyboardOnStage;
            i.applyVideoDisplaySize = applyVideoDisplaySize;
            // WebRTC controller dependencies (used by webrtcLayer.js)
            i.onRpcVideoStreamReady = onRpcVideoStreamReady;
            i.exitVideoPageAfterRemoteStreamEnded = exitVideoPageAfterRemoteStreamEnded;
            i.rpcShellCloseAllowed = rpcShellCloseAllowed;
            // Signaling helpers used by signalingLayer.js
            i.clearSessionTimer = clearSessionTimer;
            i.logDataChannel = logDataChannel;
            i.updateWebSocketState = updateWebSocketState;
            i.shouldDeferRpcShellCloseUntilVideo = shouldDeferRpcShellCloseUntilVideo;
            i.closeRpcShellOrWindow = closeRpcShellOrWindow;
        } catch (_) {}
    }

    RemoteProcessApplication.prototype.sendRequest = function () {
        const exePath = (this.session && this.session.rpcExePath) ? String(this.session.rpcExePath).trim() : '';
        if (!exePath) {
            logDataChannel(this.ui || bindStatusElements(this.doc), 'exePath 为空，无法启动远端应用');
            return;
        }
        this.session.websocket.send(JSON.stringify({ id: this.session.rpcWorkNode || 'server', type: 'request', exePath: exePath }));
    };

    RemoteProcessApplication.prototype.sendStopRequest = function () {
        if (!this.session.websocket || this.session.websocket.readyState !== WebSocket.OPEN) return;
        const policy = 'close_process';
        const idleSeconds = 10;
        this.session.websocket.send(JSON.stringify({
            id: this.session.rpcWorkNode || 'server', type: 'stop',
            closeProcess: policy === 'close_process',
            autoClose: policy === 'auto_close',
            idleSeconds: idleSeconds,
        }));
    };

    RemoteProcessApplication.prototype.start = function () {
        this.session.rpcRemoteStreamExitHandled = false;
        showVideoStage(this.session, this.doc);
        this.sendRequest();
        logDataChannel(this.ui, 'Connection started');
        this.session.rpcHadVideo = false;
        this.session.rpcVideoLastAliveAt = 0;
        armRpcNoVideoWatchdog(this.session, this.doc, this.ui);
    };

    RemoteProcessApplication.prototype.stop = function () {
        recordVideoPageExitReason(this.session, this.session && this.session.__rpc_exit_reason ? this.session.__rpc_exit_reason : 'app_stop', { detail: 'RemoteProcessApplication.stop' });
        this.sendStopRequest();
        hideVideoStage(this.doc);
        const v = getMainVideo(this.doc);
        if (v) {
            v.__rpc_keyboard_bound = false;
            v.srcObject = null;
        }
        const stg = getVideoStage(this.doc);
        if (stg) stg.__rpc_stage_mouse_bound = false;
        this.session.activeVideo = null;
        if (this.session.dc) {
            this.session.dc.close();
            this.session.dc = null;
        }
        if (this.session.pc && this.session.pc.getTransceivers) {
            this.session.pc.getTransceivers().forEach(function (t) {
                if (t.stop) t.stop();
            });
        }
        if (this.session.pc) {
            this.session.pc.getSenders().forEach(function (s) {
                if (s.track) s.track.stop();
            });
            this.session.pc.close();
            this.session.pc = null;
        }
        if (this.ui) logDataChannel(this.ui, 'Connection stopped');
        this.session.isControlEnabled = false;
        if (this.session.latencyDiag) {
            this.session.latencyDiag.dispose();
        }
        clearSessionTimer(this.session, 'rpcNoVideoTimer');
        clearSessionTimer(this.session, 'rpcWsConnectTimer');
        clearSessionTimer(this.session, 'rpcPcDisconnectTimer');
        this.session.rpcRemoteStreamExitHandled = false;
    };

    RemoteProcessApplication.prototype.bindDom = function () {
        const doc = this.doc;
        const self = this;
        this.session.activeVideo = getMainVideo(doc);

        const closeStage = doc.getElementById('video-stage-close');
        if (closeStage) {
            closeStage.addEventListener('click', function () {
                if (rpcShellCloseAllowed(self.session) && window.rpcShell && typeof window.rpcShell.close === 'function') {
                    recordVideoPageExitReason(self.session, 'ui_close_button_rpcShell', { detail: 'video-stage-close click' });
                    window.rpcShell.close();
                    return;
                }
                recordVideoPageExitReason(self.session, 'ui_close_button_hide_stage', { detail: 'video-stage-close click' });
                hideVideoStage(doc);
            });
        }

        const stage = getVideoStage(doc);
        if (stage) {
            stage.addEventListener('dblclick', function (e) {
                if (e.target === stage || e.target.id === 'video-stage') {
                    tryEnterStageFullscreen(doc);
                }
            });
        }

        let resizeTimer = null;
        window.addEventListener('resize', function () {
            if (resizeTimer) clearTimeout(resizeTimer);
            resizeTimer = setTimeout(function () {
                const st = getVideoStage(doc);
                const v = getMainVideo(doc);
                if (st && !st.hidden && v && v.srcObject) applyVideoDisplaySize(v);
            }, 120);
        });

        doc.addEventListener('keydown', function (e) {
            const st = getVideoStage(doc);
            if (!st || st.hidden) return;
            if (e.key === 'Escape') {
                e.preventDefault();
                if (rpcShellCloseAllowed(self.session) && window.rpcShell && typeof window.rpcShell.close === 'function') {
                    recordVideoPageExitReason(self.session, 'esc_rpcShell_close', { detail: 'keydown Escape' });
                    window.rpcShell.close();
                    return;
                }
                const fs = doc.fullscreenElement || doc.webkitFullscreenElement || doc.msFullscreenElement;
                if (fs) exitDocumentFullscreen(doc);
                else {
                    recordVideoPageExitReason(self.session, 'esc_hide_stage', { detail: 'keydown Escape' });
                    hideVideoStage(doc);
                }
            }
        });

        window.addEventListener('beforeunload', function () {
            recordVideoPageExitReason(self.session,
                (self.session && self.session.__rpc_exit_reason) ? self.session.__rpc_exit_reason : 'beforeunload',
                { detail: 'beforeunload' });
            hideVideoStage(doc);
        });
        window.addEventListener('pagehide', function () {
            recordVideoPageExitReason(self.session,
                (self.session && self.session.__rpc_exit_reason) ? self.session.__rpc_exit_reason : 'pagehide',
                { detail: 'pagehide' });
        });
    };

    RemoteProcessApplication.prototype.run = function () {
        this.ui = bindStatusElements(this.doc);
        if (window.__rpcWebRtc && typeof window.__rpcWebRtc.createWebRtcSessionController === 'function') {
            this.webrtc = window.__rpcWebRtc.createWebRtcSessionController(this.session, this.doc, this.ui);
        } else {
            console.error('[RemoteProcessControl] webrtcLayer 未加载，无法创建 WebRTC controller');
            this.webrtc = null;
        }
        const self = this;
        if (window.__rpcSignaling && typeof window.__rpcSignaling.createSignalingClient === 'function') {
            this.signaling = window.__rpcSignaling.createSignalingClient(this.session, this.doc, this.ui, {
                onOffer: function (msg) {
                    if (self.webrtc) return self.webrtc.handleOffer(msg);
                    return Promise.resolve();
                },
                onWebSocketOpen: function () {
                    if (!self.session.rpcAutostart) return;
                    window.setTimeout(function () {
                        if (!self.session.websocket || self.session.websocket.readyState !== WebSocket.OPEN) return;
                        if (!self.session.pc) {
                            self.start();
                        }
                    }, 80);
                },
            });
        } else {
            this.signaling = createSignalingClient(this.session, this.doc, this.ui, {
            onOffer: function (msg) {
                if (self.webrtc) return self.webrtc.handleOffer(msg);
                return Promise.resolve();
            },
            onWebSocketOpen: function () {
                if (!self.session.rpcAutostart) return;
                window.setTimeout(function () {
                    if (!self.session.websocket || self.session.websocket.readyState !== WebSocket.OPEN) return;
                    if (!self.session.pc) {
                        self.start();
                    }
                }, 80);
            },
            });
        }
        this.bindDom();
        {
            const stage = getVideoStage(this.doc);
            if (stage && !stage.hidden) {
                this.session.rpcHadVideo = false;
                this.session.rpcVideoLastAliveAt = 0;
                armRpcNoVideoWatchdog(this.session, this.doc, this.ui);
            }
        }
        if (this.session.rpcWindowMode) armRpcWebSocketConnectWatchdog(this.session);
        const connectSignaling = function () {
            self.signaling.connect();
        };
        if (typeof window.__rpcEnsureSignalingJsonLoaded === 'function') {
            window.__rpcEnsureSignalingJsonLoaded().then(connectSignaling).catch(connectSignaling);
        } else {
            connectSignaling();
        }
    };

    function startDesktopMode() {
        // 新版桌面逻辑已拆分到 desktopMode.js（支持 file:// 多文件脚本加载）。
        try {
            if (window.__rpcStartDesktopMode && typeof window.__rpcStartDesktopMode === 'function') {
                window.__rpcStartDesktopMode(document);
                return;
            }
        } catch (_) {}

        console.warn('[desktop] desktopMode.js 未加载，无法启动桌面容器。');
        return;
    }

    function startApp() {
        if (window.__rpcAppStarted) return;
        window.__rpcAppStarted = true;
        bindLayerExports();
        try {
            const params = new URLSearchParams(window.location.search);
            const rpcWindow = params.get('rpcWindow') === '1' || params.get('kiosk') === '1';
            if (rpcWindow) {
                const app = new RemoteProcessApplication(document);
                applyRpcWindowUrlFlags(app.session, document);
                window.__rpcApp = app;
                app.run();
            } else {
                if (typeof window.__rpcMountAuthPortal === 'function') {
                    window.__rpcMountAuthPortal(document, function () {
                        startDesktopMode();
                    });
                } else {
                    startDesktopMode();
                }
            }
        } catch (err) {
            console.error('[RemoteProcessControl] 启动失败:', err);
        }
    }

    // Important:
    // client.js 依赖后续 defer 脚本（uiLayer/webrtcLayer/signalingLayer）执行完成后再启动。
    // 当 readyState 为 'interactive' 时直接 startApp() 可能导致 window.__rpcSignaling 尚未就绪。
    if (document.readyState === 'complete') {
        startApp();
    } else {
        document.addEventListener('DOMContentLoaded', startApp, { once: true });
    }
})();
