/**
 * 单文件构建：兼容 file:// 双击打开（Chrome 禁止 ES Module 跨文件加载）。
 * 逻辑与 js/ 下模块版一致；开发时可继续维护 js/ 并用工具合并，或以此文件为准。
 */
(function () {
    'use strict';

    function randomId(length) {
        const characters = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
        const pickRandom = function () {
            return characters.charAt(Math.floor(Math.random() * characters.length));
        };
        return Array.from({ length: length }, pickRandom).join('');
    }

    function createTimestampState() {
        let startTime = null;
        return function currentTimestamp() {
            if (startTime === null) {
                startTime = Date.now();
                return 0;
            }
            return Date.now() - startTime;
        };
    }

    function createSession() {
        return {
            clientId: randomId(10),
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
            /** Electron 紧凑启动：先只显示记事本磁贴，双击后再进视频；与 rpcWindow 可并存 */
            electronCompactLauncher: false,
            electronVideoOpen: false,
            /** 远端进程结束/视频轨已断后已执行退出画面，避免重复 stop */
            rpcRemoteStreamExitHandled: false,
        };
    }

    function rpcShellCloseAllowed(session) {
        return session.rpcWindowMode || session.electronCompactLauncher;
    }

    function clearSessionTimer(session, key) {
        const id = session[key];
        if (id != null) {
            clearTimeout(id);
            clearInterval(id);
            session[key] = null;
        }
    }

    /**
     * Electron 紧凑启动（双击磁贴后再连）：未收到首帧前不因信令断开/ICE 失败等关窗，
     * 仅依赖「无视频超时」；出画后恢复对断流、关 track 等的自动关窗。
     */
    function shouldDeferRpcShellCloseUntilVideo(session) {
        return !!(session.electronCompactLauncher && !session.rpcHadVideo);
    }

    /** 应用模式下自动关窗（Electron 关进程；浏览器尝试 window.close） */
    function closeRpcShellOrWindow(session, reason) {
        if (!rpcShellCloseAllowed(session) || session.rpcAutoClosed) return;
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
            const startBtn = doc.getElementById('start');
            const stopBtn = doc.getElementById('stop');
            if (startBtn) startBtn.disabled = false;
            if (stopBtn) stopBtn.disabled = true;
            if (ui) logDataChannel(ui, '远端已无视频流，已退出画面（' + reason + '）');
        }
    }

    function forceExitVideoPageOnNoVideoTimeout(session, doc, ui, reason) {
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
        if ((!session.rpcWindowMode && !session.electronCompactLauncher) || session.rpcAutoClosed) return;
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
        if (rpcWindow) {
            const tmo = parseInt(params.get('rpcVideoTimeoutMs') || '60000', 10);
            session.rpcVideoTimeoutMs = !isNaN(tmo) && tmo >= 3000 ? tmo : 60000;

            // Pass app exe path from URL for multi-window desktop container.
            const exePathParam = params.get('exePath');
            if (exePathParam && String(exePathParam).trim() !== '') {
                session.rpcExePath = exePathParam;
                const exeEl = doc.getElementById('exe-path');
                if (exeEl) exeEl.value = exePathParam;
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

    /**
     * Electron（preload 注入 isElectron）：默认 electronCompact=1、autostart=0，先只显示记事本磁贴，双击再连接。
     * 无边框窗口由 Electron main 的 frame:false 实现。
     */
    function applyElectronShellFlags(session, doc) {
        if (!window.rpcShell || !window.rpcShell.isElectron) return;
        const params = new URLSearchParams(window.location.search);
        if (params.get('electronCompact') === '0') return;
        session.electronCompactLauncher = true;
        doc.documentElement.classList.add('electron-compact-launcher', 'electron-frameless');
        if (!session.rpcWindowMode) {
            if (params.get('autostart') === '1') {
                session.rpcAutostart = true;
            } else if (params.get('autostart') === '0') {
                session.rpcAutostart = false;
            } else {
                session.rpcAutostart = false;
            }
            if (!params.get('windowTitle')) {
                doc.title = '远程应用';
            }
        }
        /* 双击后再连：默认 60s 内须出画；信令未连上仍维持短超时（可用 URL 覆盖） */
        const vtParam = params.get('rpcVideoTimeoutMs');
        if (vtParam != null && String(vtParam).trim() !== '') {
            const vt = parseInt(vtParam, 10);
            if (!isNaN(vt) && vt >= 3000) session.rpcVideoTimeoutMs = vt;
        } else {
            session.rpcVideoTimeoutMs = 60000;
        }
        const wtParam = params.get('rpcWsConnectTimeoutMs');
        if (wtParam != null && String(wtParam).trim() !== '') {
            const wt = parseInt(wtParam, 10);
            if (!isNaN(wt) && wt >= 3000) session.rpcWsConnectTimeoutMs = wt;
        } else {
            session.rpcWsConnectTimeoutMs = 10000;
        }
    }

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

    function keyboardEventToWindowsVk(ev) {
        if (window.__rpcUI && typeof window.__rpcUI.keyboardEventToWindowsVk === 'function') {
            return window.__rpcUI.keyboardEventToWindowsVk(ev);
        }
        return 0;
    }

    function pointerToVideoPixels(video, clientX, clientY, streamW, streamH) {
        if (window.__rpcUI && typeof window.__rpcUI.pointerToVideoPixels === 'function') {
            return window.__rpcUI.pointerToVideoPixels(video, clientX, clientY, streamW, streamH);
        }
        return null;
    }

    function bindStatusElements(doc) {
        return {
            iceConnectionState: doc.getElementById('ice-connection-state'),
            iceGatheringState: doc.getElementById('ice-gathering-state'),
            signalingState: doc.getElementById('signaling-state'),
            dataChannelState: doc.getElementById('data-channel-state'),
            dataChannelLog: doc.getElementById('data-channel-log'),
            websocketState: doc.getElementById('websocket-state'),
            websocketIndicator: doc.getElementById('websocket-indicator'),
            iceConnectionIndicator: doc.getElementById('ice-connection-indicator'),
            dataChannelIndicator: doc.getElementById('data-channel-indicator'),
            captureHealthState: doc.getElementById('capture-health-state'),
        };
    }

    function logDataChannel(ui, message) {
        if (!ui || !ui.dataChannelLog) return;
        const timestamp = new Date().toLocaleTimeString();
        ui.dataChannelLog.textContent += '[' + timestamp + '] ' + message + '\n';
        ui.dataChannelLog.scrollTop = ui.dataChannelLog.scrollHeight;
    }

    function updateWebSocketState(ui, state) {
        if (!ui) return;
        if (ui.websocketState) ui.websocketState.textContent = state;
        if (ui.websocketIndicator) {
            ui.websocketIndicator.className = 'status-indicator ' + (state === 'connected' ? 'active' : '');
        }
    }

    function updateDataChannelState(ui, state) {
        if (!ui) return;
        if (ui.dataChannelState) ui.dataChannelState.textContent = state;
        if (ui.dataChannelIndicator) {
            ui.dataChannelIndicator.className = 'status-indicator ' + (state === 'open' ? 'active' : '');
        }
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

    // Export a minimal internal API for pass-through layer files.
    // Next step will be to physically move code out of this file gradually.
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

            if (window.__rpcUI && typeof window.__rpcUI._bindFromInternal === 'function') window.__rpcUI._bindFromInternal();
            if (window.__rpcWebRtc && typeof window.__rpcWebRtc._bindFromInternal === 'function') window.__rpcWebRtc._bindFromInternal();
            if (window.__rpcSignaling && typeof window.__rpcSignaling._bindFromInternal === 'function') window.__rpcSignaling._bindFromInternal();
        } catch (_) {}
    }

    RemoteProcessApplication.prototype.sendRequest = function () {
        const exeEl = this.doc.getElementById('exe-path');
        const exePath = (this.session && this.session.rpcExePath) ? String(this.session.rpcExePath).trim()
            : ((exeEl && exeEl.value) ? exeEl.value.trim() : '');
        if (!exePath) {
            logDataChannel(this.ui || bindStatusElements(this.doc), 'exePath 为空，无法启动远端应用');
            return;
        }
        this.session.websocket.send(JSON.stringify({ id: 'server', type: 'request', exePath: exePath }));
    };

    RemoteProcessApplication.prototype.sendStopRequest = function () {
        if (!this.session.websocket || this.session.websocket.readyState !== WebSocket.OPEN) return;
        const policyEl = this.doc.getElementById('stop-policy');
        const policy = policyEl ? policyEl.value : 'close_process';
        const idleEl = this.doc.getElementById('idle-seconds');
        const idleRaw = (idleEl && idleEl.value) || '10';
        const idleSeconds = Math.max(0, parseInt(String(idleRaw).trim(), 10) || 0);
        this.session.websocket.send(JSON.stringify({
            id: 'server', type: 'stop',
            closeProcess: policy === 'close_process',
            autoClose: policy === 'auto_close',
            idleSeconds: idleSeconds,
        }));
    };

    RemoteProcessApplication.prototype.start = function () {
        this.session.rpcRemoteStreamExitHandled = false;
        showVideoStage(this.session, this.doc);
        const startBtn = this.doc.getElementById('start');
        const stopBtn = this.doc.getElementById('stop');
        if (startBtn) startBtn.disabled = true;
        if (stopBtn) stopBtn.disabled = false;
        this.sendRequest();
        logDataChannel(this.ui, 'Connection started');
        this.session.rpcHadVideo = false;
        this.session.rpcVideoLastAliveAt = 0;
        armRpcNoVideoWatchdog(this.session, this.doc, this.ui);
        if (this.session.electronCompactLauncher && !this.session.rpcAutostart) {
            armRpcWebSocketConnectWatchdog(this.session);
        }
    };

    RemoteProcessApplication.prototype.stop = function () {
        this.sendStopRequest();
        const startBtn = this.doc.getElementById('start');
        const stopBtn = this.doc.getElementById('stop');
        if (startBtn) startBtn.disabled = false;
        if (stopBtn) stopBtn.disabled = true;
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
        if (this.ui) {
            if (this.ui.iceConnectionIndicator) this.ui.iceConnectionIndicator.className = 'status-indicator';
            if (this.ui.dataChannelIndicator) this.ui.dataChannelIndicator.className = 'status-indicator';
            logDataChannel(this.ui, 'Connection stopped');
        }
        this.session.isControlEnabled = false;
        this.session.electronVideoOpen = false;
        this.doc.documentElement.classList.remove('electron-video-active');
        if (this.session.electronCompactLauncher && !this.session.rpcWindowMode) {
            this.doc.documentElement.classList.remove('rpc-window-mode');
        }
        if (this.session.latencyDiag) {
            this.session.latencyDiag.dispose();
        }
        clearSessionTimer(this.session, 'rpcNoVideoTimer');
        clearSessionTimer(this.session, 'rpcWsConnectTimer');
        clearSessionTimer(this.session, 'rpcPcDisconnectTimer');
        this.session.rpcRemoteStreamExitHandled = false;
    };

    RemoteProcessApplication.prototype.onLaunchTileDoubleClick = function () {
        if (!this.session.websocket || this.session.websocket.readyState !== WebSocket.OPEN) {
            logDataChannel(this.ui || bindStatusElements(this.doc), '请先等待 WebSocket 连接成功');
            return;
        }
        if (!this.session.pc) {
            this.start();
        } else {
            showVideoStage(this.session, this.doc);
            armRpcNoVideoWatchdog(this.session, this.doc, this.ui);
            if (this.webrtc) this.webrtc.reattachVideoToMain();
        }
    };

    /** 将磁贴 data-rpc-exe 写入 exe-path 后执行与双击磁贴相同的连接逻辑 */
    RemoteProcessApplication.prototype.applyTileExeAndLaunch = function (tileEl) {
        if (!tileEl) return;
        const path = tileEl.getAttribute('data-rpc-exe');
        const exeEl = this.doc.getElementById('exe-path');
        if (path && exeEl) exeEl.value = path;
        this.onLaunchTileDoubleClick();
    };

    RemoteProcessApplication.prototype.bindDom = function () {
        const doc = this.doc;
        const self = this;
        this.session.activeVideo = getMainVideo(doc);

        const enableRc = doc.getElementById('enable-remote-control');
        if (enableRc) {
            enableRc.addEventListener('change', function (e) {
                self.session.remoteControlEnabled = e.target.checked;
                logDataChannel(self.ui, 'Remote control ' + (self.session.remoteControlEnabled ? 'enabled' : 'disabled'));
            });
        }

        const stopPolicy = doc.getElementById('stop-policy');
        if (stopPolicy) {
            stopPolicy.addEventListener('change', function (e) {
                const row = doc.getElementById('idle-seconds-row');
                if (row) row.style.display = e.target.value === 'auto_close' ? 'flex' : 'none';
            });
        }

        const toggleExe = doc.getElementById('toggle-exe-details');
        if (toggleExe) {
            toggleExe.addEventListener('click', function () {
                const panel = doc.getElementById('exe-details-panel');
                if (!panel) return;
                const open = panel.style.display !== 'none';
                panel.style.display = open ? 'none' : 'block';
            });
        }

        doc.querySelectorAll('.app-launch-tile[data-rpc-exe]').forEach(function (tile) {
            tile.addEventListener('dblclick', function (e) {
                e.preventDefault();
                self.applyTileExeAndLaunch(tile);
            });
            tile.addEventListener('keydown', function (e) {
                if (e.key === 'Enter' || e.key === ' ') {
                    e.preventDefault();
                    self.applyTileExeAndLaunch(tile);
                }
            });
        });

        const startBtn = doc.getElementById('start');
        if (startBtn) startBtn.addEventListener('click', function () { self.start(); });
        const stopBtnEl = doc.getElementById('stop');
        if (stopBtnEl) stopBtnEl.addEventListener('click', function () { self.stop(); });

        const closeStage = doc.getElementById('video-stage-close');
        if (closeStage) {
            closeStage.addEventListener('click', function () {
                if (rpcShellCloseAllowed(self.session) && window.rpcShell && typeof window.rpcShell.close === 'function') {
                    window.rpcShell.close();
                    return;
                }
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
            if (e.key === 'Escape' && self.session.electronCompactLauncher) {
                const st0 = getVideoStage(doc);
                if (!st0 || st0.hidden) {
                    e.preventDefault();
                    if (window.rpcShell && typeof window.rpcShell.close === 'function') {
                        window.rpcShell.close();
                    }
                    return;
                }
            }
            const st = getVideoStage(doc);
            if (!st || st.hidden) return;
            if (e.key === 'Escape') {
                e.preventDefault();
                if (rpcShellCloseAllowed(self.session) && window.rpcShell && typeof window.rpcShell.close === 'function') {
                    window.rpcShell.close();
                    return;
                }
                const fs = doc.fullscreenElement || doc.webkitFullscreenElement || doc.msFullscreenElement;
                if (fs) exitDocumentFullscreen(doc);
                else hideVideoStage(doc);
            }
        });

        if (stopBtnEl) stopBtnEl.disabled = true;
        window.addEventListener('beforeunload', function () { hideVideoStage(doc); });
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
        /* 紧凑模式且手动双击再连：勿在启动页就开始「信令 10s 未连上关窗」，改到 start() 再计时 */
        const deferWsWatchdog =
            this.session.electronCompactLauncher && !this.session.rpcAutostart;
        if ((this.session.rpcWindowMode || this.session.electronCompactLauncher) && !deferWsWatchdog) {
            armRpcWebSocketConnectWatchdog(this.session);
        }
        this.signaling.connect();
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
        // Ensure layer pass-through files can bind to client.js internals.
        bindLayerExports();
        try {
            const params = new URLSearchParams(window.location.search);
            const rpcWindow = params.get('rpcWindow') === '1' || params.get('kiosk') === '1';
            const useLegacyUi = params.get('rpcLegacy') === '1' || params.get('desktop') === '0';
            if (rpcWindow || useLegacyUi) {
                const app = new RemoteProcessApplication(document);
                applyRpcWindowUrlFlags(app.session, document);
                applyElectronShellFlags(app.session, document);
                window.__rpcApp = app;
                app.run();
            } else {
                startDesktopMode();
            }
        } catch (err) {
            console.error('[RemoteProcessControl] 启动失败:', err);
            const pre = document.getElementById('data-channel-log');
            if (pre) {
                pre.textContent += '\n[错误] 前端脚本无法启动: ' + (err && err.message ? err.message : err) + '\n';
            }
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
