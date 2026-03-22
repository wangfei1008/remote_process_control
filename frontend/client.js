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
            /** 独立窗口/应用模式：隐藏网页控制台，仅全屏远程画面（由 URL ?rpcWindow=1 开启） */
            rpcWindowMode: false,
            /** WebSocket 连通后自动 Start（应用模式默认 true，可用 autostart=0 关闭） */
            rpcAutostart: false,
            rpcHadVideo: false,
            rpcAutoClosed: false,
            /** 应用模式：多久内未收到有效视频则关窗（ms），URL ?rpcVideoTimeoutMs= */
            rpcVideoTimeoutMs: 90000,
            /** 应用模式：WebSocket 多久未连上则关窗（ms），URL ?rpcWsConnectTimeoutMs= */
            rpcWsConnectTimeoutMs: 45000,
            rpcNoVideoTimer: null,
            rpcWsConnectTimer: null,
            rpcPcDisconnectTimer: null,
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

    function armRpcNoVideoWatchdog(session) {
        if ((!session.rpcWindowMode && !session.electronCompactLauncher) || session.rpcAutoClosed) return;
        clearSessionTimer(session, 'rpcNoVideoTimer');
        const ms = Math.max(3000, Number(session.rpcVideoTimeoutMs) || 90000);
        session.rpcNoVideoTimer = setTimeout(function () {
            session.rpcNoVideoTimer = null;
            if (!session.rpcHadVideo) {
                closeRpcShellOrWindow(session, 'timeout_no_video_' + ms + 'ms');
            }
        }, ms);
    }

    function clearRpcNoVideoWatchdog(session) {
        clearSessionTimer(session, 'rpcNoVideoTimer');
    }

    function onRpcVideoStreamReady(session) {
        session.rpcHadVideo = true;
        clearRpcNoVideoWatchdog(session);
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
            const tmo = parseInt(params.get('rpcVideoTimeoutMs') || '90000', 10);
            session.rpcVideoTimeoutMs = !isNaN(tmo) && tmo >= 3000 ? tmo : 90000;
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
        /* 双击后再连：默认 10s 内须出画；信令未连上也用同档超时（可用 URL 覆盖） */
        const vtParam = params.get('rpcVideoTimeoutMs');
        if (vtParam != null && String(vtParam).trim() !== '') {
            const vt = parseInt(vtParam, 10);
            if (!isNaN(vt) && vt >= 3000) session.rpcVideoTimeoutMs = vt;
        } else {
            session.rpcVideoTimeoutMs = 10000;
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
        if (!ev || ev.isComposing) return 0;
        const c = ev.code;
        if (!c) return typeof ev.keyCode === 'number' ? ev.keyCode : 0;
        const oem = {
            Minus: 189, Equal: 187, BracketLeft: 219, BracketRight: 221, Backslash: 220,
            Semicolon: 186, Quote: 222, Backquote: 192, Comma: 188, Period: 190, Slash: 191,
            IntlBackslash: 220,
        };
        if (oem[c] !== undefined) return oem[c];
        const named = {
            Backspace: 8, Tab: 9, Enter: 13, NumpadEnter: 13,
            ShiftLeft: 16, ShiftRight: 16, ControlLeft: 17, ControlRight: 17,
            AltLeft: 18, AltRight: 18, Pause: 19, CapsLock: 20, Escape: 27, Space: 32,
            PageUp: 33, PageDown: 34, End: 35, Home: 36,
            ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40,
            PrintScreen: 44, Insert: 45, Delete: 46,
            MetaLeft: 91, MetaRight: 92, ContextMenu: 93,
            ScrollLock: 145, NumLock: 144,
        };
        if (named[c] !== undefined) return named[c];
        if (c.length === 4 && c.indexOf('Key') === 0) {
            const ch = c.charCodeAt(3);
            if (ch >= 65 && ch <= 90) return ch;
        }
        if (c.length === 6 && c.indexOf('Digit') === 0) {
            const d = c.charCodeAt(5) - 48;
            if (d >= 0 && d <= 9) return 48 + d;
        }
        const fm = /^F(\d{1,2})$/.exec(c);
        if (fm) {
            const fn = parseInt(fm[1], 10);
            if (fn >= 1 && fn <= 24) return 111 + fn;
        }
        if (c.indexOf('Numpad') === 0) {
            if (c === 'Numpad0') return 96;
            if (c.length === 7 && c.charAt(6) >= '1' && c.charAt(6) <= '9')
                return 96 + parseInt(c.charAt(6), 10);
            if (c === 'NumpadDecimal') return 110;
            if (c === 'NumpadAdd') return 107;
            if (c === 'NumpadSubtract') return 109;
            if (c === 'NumpadMultiply') return 106;
            if (c === 'NumpadDivide') return 111;
        }
        return typeof ev.keyCode === 'number' ? ev.keyCode : 0;
    }

    function pointerToVideoPixels(video, clientX, clientY, streamW, streamH) {
        const rect = video.getBoundingClientRect();
        let iw = video.videoWidth;
        let ih = video.videoHeight;
        if (!iw || !ih) {
            iw = streamW;
            ih = streamH;
        }
        if (!iw || !ih) return null;
        const rw = rect.width;
        const rh = rect.height;
        const scale = Math.min(rw / iw, rh / ih);
        const contentW = iw * scale;
        const contentH = ih * scale;
        const offsetX = (rw - contentW) / 2;
        const offsetY = (rh - contentH) / 2;
        let px = clientX - rect.left - offsetX;
        let py = clientY - rect.top - offsetY;
        px = Math.max(0, Math.min(contentW, px));
        py = Math.max(0, Math.min(contentH, py));
        return {
            absoluteX: Math.round((px / contentW) * iw),
            absoluteY: Math.round((py / contentH) * ih),
            videoWidth: iw,
            videoHeight: ih,
        };
    }

    function smoothEwma(prev, sample, alpha) {
        if (prev == null || isNaN(prev)) return sample;
        return prev * (1 - alpha) + sample * alpha;
    }

    /**
     * 延时诊断：DataChannel 对时(latPing/latPong)、frameMark、getStats(抖动缓冲/解码/ICE RTT)、requestVideoFrameCallback。
     * 总延时为粗算：发送端采集+编码 + 编码结束→DC收到 + 抖动缓冲均值 + 解码均值（详见控制台 [latency] 日志）。
     */
    function createLatencyDiagnostics(session, doc) {
        const state = {
            thetaMs: null,
            rttMs: null,
            lastDcLagMs: null,
            lastMarkCapMs: null,
            lastMarkEncMs: null,
            lastMarkSeq: null,
            rvfHandle: null,
            pingTimer: null,
            statsTimer: null,
            videoEl: null,
            pingSeq: 0,
        };

        function hudText(text) {
            const el = doc.getElementById('rpc-latency-hud');
            if (el) el.textContent = text;
        }

        function tryHandleMessage(str) {
            try {
                const j = JSON.parse(str);
                if (!j || typeof j.type !== 'string') return false;
                if (j.type === 'latPong') {
                    const t2 = Date.now();
                    const tCli = Number(j.tCli) || 0;
                    const rtt = Math.max(0, t2 - tCli);
                    const tSrv = Number(j.tSrv) || 0;
                    const theta = (tSrv + rtt / 2) - t2;
                    state.thetaMs = smoothEwma(state.thetaMs, theta, 0.25);
                    state.rttMs = smoothEwma(state.rttMs, rtt, 0.25);
                    console.info('[latency] latPong RTT≈' + Math.round(rtt) + 'ms theta≈' + Math.round(theta)
                        + 'ms（服务端与浏览器时钟换算，用于 frameMark）');
                    return true;
                }
                if (j.type === 'frameMark') {
                    const tRecv = Date.now();
                    const srvMs = Number(j.srvMs) || 0;
                    const th = state.thetaMs;
                    let dcLag = null;
                    if (th != null && !isNaN(th)) {
                        dcLag = tRecv - srvMs + th;
                    }
                    state.lastDcLagMs = dcLag;
                    state.lastMarkCapMs = j.capMs;
                    state.lastMarkEncMs = j.encMs;
                    state.lastMarkSeq = j.seq;
                    if (dcLag != null && !isNaN(dcLag)) {
                        console.info('[latency] frameMark seq=' + j.seq
                            + ' 发送端 cap+enc_ms=' + j.capMs + '+' + j.encMs
                            + ' 编码结束→本机收到DC≈' + Math.round(dcLag) + 'ms（含 RTP/网络/浏览器）');
                    } else {
                        console.info('[latency] frameMark seq=' + j.seq + ' cap=' + j.capMs + ' enc=' + j.encMs
                            + '（等待 latPing 对时后可算链路延时）');
                    }
                    return true;
                }
            } catch (_) {
                return false;
            }
            return false;
        }

        function sendLatPing() {
            if (!session.dc || session.dc.readyState !== 'open') return;
            state.pingSeq += 1;
            session.dc.send(JSON.stringify({ type: 'latPing', seq: state.pingSeq, tCli: Date.now() }));
        }

        function pollStatsImpl(pc) {
            if (!pc) return;
            const p = pc.getStats(null);
            if (!p || typeof p.then !== 'function') return;
            p.then(function (report) {
                let inboundVideo = null;
                let candPair = null;
                report.forEach(function (s) {
                    if (s.type === 'inbound-rtp' && (s.kind === 'video' || s.mediaType === 'video')) {
                        inboundVideo = s;
                    }
                    if (s.type === 'candidate-pair' && s.state === 'succeeded' && s.nominated) {
                        candPair = s;
                    }
                });
                if (!candPair) {
                    report.forEach(function (s) {
                        if (s.type === 'candidate-pair' && s.state === 'succeeded') candPair = s;
                    });
                }

                const parts = [];
                let jitterBufMs = null;
                let decodeMsPerFrame = null;
                if (inboundVideo) {
                    const jbd = inboundVideo.jitterBufferDelay;
                    const jbec = inboundVideo.jitterBufferEmittedCount;
                    if (typeof jbd === 'number' && typeof jbec === 'number' && jbec > 0) {
                        jitterBufMs = (jbd / jbec) * 1000;
                        parts.push('抖动缓冲均值≈' + Math.round(jitterBufMs) + 'ms');
                    }
                    const td = inboundVideo.totalDecodeTime;
                    const fd = inboundVideo.framesDecoded;
                    if (typeof td === 'number' && typeof fd === 'number' && fd > 0) {
                        decodeMsPerFrame = (td / fd) * 1000;
                        parts.push('解码均值≈' + decodeMsPerFrame.toFixed(1) + 'ms/帧');
                    }
                    if (typeof inboundVideo.jitter === 'number') {
                        parts.push('RTP抖动=' + (inboundVideo.jitter * 1000).toFixed(2) + 'ms');
                    }
                    if (typeof inboundVideo.framesDropped === 'number') {
                        parts.push('framesDropped=' + inboundVideo.framesDropped);
                    }
                }
                let rttMsIce = null;
                if (candPair && typeof candPair.currentRoundTripTime === 'number') {
                    rttMsIce = candPair.currentRoundTripTime * 1000;
                    parts.push('ICE_RTT≈' + Math.round(rttMsIce) + 'ms');
                }

                const cap = state.lastMarkCapMs != null ? Number(state.lastMarkCapMs) : 0;
                const enc = state.lastMarkEncMs != null ? Number(state.lastMarkEncMs) : 0;
                const senderPipe = (cap > 0 || enc > 0) ? (cap + enc) : 0;
                const dcLag = state.lastDcLagMs;
                let totalEst = null;
                if (dcLag != null && !isNaN(dcLag) && jitterBufMs != null) {
                    totalEst = senderPipe + dcLag + jitterBufMs + (decodeMsPerFrame || 0);
                }

                let line = '[latency][browser] ' + (parts.length ? parts.join(' | ') : '(无 inbound-rtp 视频统计)');
                if (senderPipe > 0) line += ' | 发送端采集+编码(最近frameMark)=' + Math.round(senderPipe) + 'ms';
                if (dcLag != null && !isNaN(dcLag)) line += ' | 编码结束→DC收到≈' + Math.round(dcLag) + 'ms';
                if (totalEst != null && !isNaN(totalEst)) {
                    line += ' | 粗算总延时≈' + Math.round(totalEst) + 'ms';
                    line += ' (=采集+编码+编码→DC+RTP侧抖动缓冲+解码；不含 <video> 合成/显示器)';
                }
                console.info(line);

                const shortHud = [];
                if (rttMsIce != null) shortHud.push('RTT' + Math.round(rttMsIce));
                if (jitterBufMs != null) shortHud.push('JB' + Math.round(jitterBufMs));
                if (dcLag != null && !isNaN(dcLag)) shortHud.push('链' + Math.round(dcLag));
                if (totalEst != null) shortHud.push('Σ~' + Math.round(totalEst));
                hudText(shortHud.length ? ('延时: ' + shortHud.join(' ')) : '延时: 等对时…');
            }).catch(function (e) {
                console.warn('[latency] getStats 失败', e);
            });
        }

        function scheduleVideoFrameHook(videoEl) {
            if (!videoEl || typeof videoEl.requestVideoFrameCallback !== 'function') return;
            function onFrame(now, meta) {
                if (meta && typeof meta.presentationTime === 'number' && typeof now === 'number') {
                    const slip = now - meta.presentationTime;
                    if (!state._rvfLogNext || Date.now() > state._rvfLogNext) {
                        state._rvfLogNext = Date.now() + 5000;
                        console.info('[latency] requestVideoFrameCallback presentation 与 now 差≈' + slip.toFixed(1) + 'ms（浏览器合成层）');
                    }
                }
                if (state.videoEl === videoEl) {
                    state.rvfHandle = videoEl.requestVideoFrameCallback(onFrame);
                }
            }
            if (state.videoEl && state.rvfHandle != null && typeof state.videoEl.cancelVideoFrameCallback === 'function') {
                try { state.videoEl.cancelVideoFrameCallback(state.rvfHandle); } catch (_) {}
            }
            state.videoEl = videoEl;
            state.rvfHandle = videoEl.requestVideoFrameCallback(onFrame);
        }

        return {
            tryHandleMessage: tryHandleMessage,
            onDataChannelOpen: function () {
                if (state.pingTimer) clearInterval(state.pingTimer);
                state.pingTimer = setInterval(sendLatPing, 3000);
                sendLatPing();
            },
            bindPeer: function (pc) {
                if (state.statsTimer) clearInterval(state.statsTimer);
                state.statsTimer = setInterval(function () {
                    pollStatsImpl(pc);
                }, 2000);
                pollStatsImpl(pc);
            },
            bindVideo: function (videoEl) {
                scheduleVideoFrameHook(videoEl);
            },
            dispose: function () {
                if (state.pingTimer) clearInterval(state.pingTimer);
                if (state.statsTimer) clearInterval(state.statsTimer);
                state.pingTimer = null;
                state.statsTimer = null;
                if (state.videoEl && state.rvfHandle != null && typeof state.videoEl.cancelVideoFrameCallback === 'function') {
                    try { state.videoEl.cancelVideoFrameCallback(state.rvfHandle); } catch (_) {}
                }
                state.rvfHandle = null;
                state.videoEl = null;
                hudText('延时: —');
            },
        };
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
        return doc.getElementById('rpc-main-video');
    }

    function getVideoStage(doc) {
        return doc.getElementById('video-stage');
    }

    function applyVideoDisplaySize(video) {
        if (!video) return;
        const iw = video.videoWidth;
        const ih = video.videoHeight;
        if (!iw || !ih) return;
        const doc = video.ownerDocument;
        const compact = doc && doc.documentElement.classList.contains('rpc-window-mode');
        /* 应用/全屏模式 HUD 已藏，不必留过大边距，否则易被误认为「画面不全」 */
        const marginX = compact ? 16 : 40;
        const marginY = compact ? 20 : 100;
        const maxW = Math.max(160, window.innerWidth - marginX);
        const maxH = Math.max(120, window.innerHeight - marginY);
        let dw = iw;
        let dh = ih;
        if (dw > maxW) {
            const s = maxW / dw;
            dw = Math.round(dw * s);
            dh = Math.round(dh * s);
        }
        if (dh > maxH) {
            const s = maxH / dh;
            dw = Math.round(dw * s);
            dh = Math.round(dh * s);
        }
        video.style.width = dw + 'px';
        video.style.height = dh + 'px';
    }

    function tryEnterStageFullscreen(doc) {
        const stage = getVideoStage(doc);
        if (!stage || stage.hidden) return;
        try {
            if (typeof stage.requestFullscreen === 'function') {
                const p = stage.requestFullscreen({ navigationUI: 'hide' });
                if (p && typeof p.catch === 'function') p.catch(function () {});
            } else if (typeof stage.webkitRequestFullscreen === 'function') {
                stage.webkitRequestFullscreen();
            } else if (typeof stage.msRequestFullscreen === 'function') {
                stage.msRequestFullscreen();
            }
        } catch (_) {}
    }

    function exitDocumentFullscreen(doc) {
        const ex = doc.exitFullscreen || doc.webkitExitFullscreen || doc.msExitFullscreen;
        if (ex) {
            try {
                ex.call(doc);
            } catch (_) {}
        }
    }

    function showVideoStage(session, doc) {
        const stage = getVideoStage(doc);
        if (!stage) return;
        stage.hidden = false;
        if (session.electronCompactLauncher) {
            session.electronVideoOpen = true;
            doc.documentElement.classList.add('rpc-window-mode', 'electron-video-active');
            const splash = doc.getElementById('rpc-window-wait');
            if (splash) splash.hidden = true;
        }
        session.activeVideo = getMainVideo(doc);
        if (session.activeVideo && session.activeVideo.srcObject) {
            session.activeVideo.play().catch(function () {});
            applyVideoDisplaySize(session.activeVideo);
        }
    }

    function hideVideoStage(doc) {
        const stage = getVideoStage(doc);
        if (!stage) return;
        exitDocumentFullscreen(doc);
        stage.hidden = true;
        doc.body.style.overflow = '';
    }

    function updateVideoSizeInfo(doc) {
        const v = getMainVideo(doc);
        const el = doc.getElementById('rpc-video-size');
        if (el && v) el.textContent = v.videoWidth + ' x ' + v.videoHeight;
    }

    function canSendControl(session) {
        return session.remoteControlEnabled && session.dc && session.dc.readyState === 'open';
    }

    function isEventOnVideoHud(target) {
        return target && target.closest && target.closest('.video-stage-hud');
    }

    function sendMouseMoveFromEvent(session, video, event) {
        if (!session.dc || !video) return;
        const p = pointerToVideoPixels(
            video, event.clientX, event.clientY, session.rpcStreamW, session.rpcStreamH,
        );
        if (!p) return;
        session.dc.send(JSON.stringify({
            type: 'mouseMove', x: 0, y: 0,
            absoluteX: p.absoluteX, absoluteY: p.absoluteY,
            videoWidth: p.videoWidth, videoHeight: p.videoHeight,
        }));
    }

    function setupStageMouse(session, doc) {
        const stage = getVideoStage(doc);
        const video = getMainVideo(doc);
        if (!stage || !video || stage.__rpc_stage_mouse_bound) return;
        stage.__rpc_stage_mouse_bound = true;
        const d = video.ownerDocument;
        let lastX = null;
        let lastY = null;
        const mapButton = function (b) { return b === 0 ? 0 : b === 2 ? 1 : 2; };

        stage.addEventListener('mousemove', function (event) {
            if (isEventOnVideoHud(event.target)) return;
            if (!canSendControl(session)) return;
            const p = pointerToVideoPixels(
                video, event.clientX, event.clientY, session.rpcStreamW, session.rpcStreamH,
            );
            if (!p) return;
            const posEl = d.getElementById('rpc-cursor-pos');
            if (posEl) posEl.textContent = p.absoluteX + ', ' + p.absoluteY;
            const dx = lastX === null ? 0 : p.absoluteX - lastX;
            const dy = lastY === null ? 0 : p.absoluteY - lastY;
            lastX = p.absoluteX;
            lastY = p.absoluteY;
            session.dc.send(JSON.stringify({
                type: 'mouseMove',
                x: Math.round(dx), y: Math.round(dy),
                absoluteX: p.absoluteX, absoluteY: p.absoluteY,
                videoWidth: p.videoWidth, videoHeight: p.videoHeight,
            }));
        });

        stage.addEventListener('mousedown', function (event) {
            if (isEventOnVideoHud(event.target)) return;
            if (!canSendControl(session)) return;
            event.preventDefault();
            stage.setAttribute('tabindex', '-1');
            stage.focus();
            sendMouseMoveFromEvent(session, video, event);
            session.dc.send(JSON.stringify({
                type: 'mouseDown', button: mapButton(event.button), x: 0, y: 0,
            }));
        });

        stage.addEventListener('mouseup', function (event) {
            if (isEventOnVideoHud(event.target)) return;
            if (!canSendControl(session)) return;
            event.preventDefault();
            sendMouseMoveFromEvent(session, video, event);
            session.dc.send(JSON.stringify({
                type: 'mouseUp', button: mapButton(event.button), x: 0, y: 0,
            }));
        });

        stage.addEventListener('dblclick', function (event) {
            if (isEventOnVideoHud(event.target)) return;
            if (!canSendControl(session)) return;
            event.preventDefault();
            sendMouseMoveFromEvent(session, video, event);
            session.dc.send(JSON.stringify({
                type: 'mouseDoubleClick', button: mapButton(event.button), x: 0, y: 0,
            }));
        });

        stage.addEventListener('wheel', function (event) {
            if (isEventOnVideoHud(event.target)) return;
            if (!canSendControl(session)) return;
            event.preventDefault();
            session.dc.send(JSON.stringify({
                type: 'mouseWheel',
                deltaX: Math.round(event.deltaX), deltaY: Math.round(event.deltaY),
                x: 0, y: 0,
            }));
        }, { passive: false });

        stage.addEventListener('contextmenu', function (event) {
            if (isEventOnVideoHud(event.target)) return;
            event.preventDefault();
        });
    }

    function setupKeyboardOnStage(session, doc) {
        const stage = getVideoStage(doc);
        if (!stage || stage.__rpc_keyboard_bound) return;
        stage.__rpc_keyboard_bound = true;
        stage.setAttribute('tabindex', '-1');

        function sendKey(type, event) {
            if (isEventOnVideoHud(event.target)) return;
            if (!canSendControl(session)) return;
            if (event.key === 'Escape') return;
            const vk = keyboardEventToWindowsVk(event);
            if (!vk) return;
            event.preventDefault();
            session.dc.send(JSON.stringify({
                type: type, vk: vk, key: event.key || '', code: event.code || '',
                keyCode: typeof event.keyCode === 'number' ? event.keyCode : 0,
                shiftKey: event.shiftKey ? 1 : 0, ctrlKey: event.ctrlKey ? 1 : 0,
                altKey: event.altKey ? 1 : 0, metaKey: event.metaKey ? 1 : 0,
            }));
        }

        stage.addEventListener('keydown', function (e) { sendKey('keyDown', e); });
        stage.addEventListener('keyup', function (e) { sendKey('keyUp', e); });
    }

    function setupRemoteControl(session, doc) {
        const video = getMainVideo(doc);
        if (!video) return;
        setupStageMouse(session, doc);
        setupKeyboardOnStage(session, doc);
    }

    function createWebRtcSessionController(session, doc, ui) {
        const currentTimestamp = createTimestampState();
        const latencyDiag = createLatencyDiagnostics(session, doc);
        session.latencyDiag = latencyDiag;

        function attachStreamToMainVideo(videoEl, mediaStream) {
            if (!videoEl || !mediaStream) return;
            session.activeVideo = videoEl;
            session.activeVideo.__rpc_keyboard_bound = false;
            const st = doc.getElementById('video-stage');
            if (st) st.__rpc_stage_mouse_bound = false;
            session.activeVideo.defaultMuted = true;
            session.activeVideo.muted = true;
            session.activeVideo.setAttribute('playsinline', '');
            session.activeVideo.setAttribute('webkit-playsinline', '');
            session.activeVideo.srcObject = mediaStream;
            const vtrack = mediaStream.getVideoTracks && mediaStream.getVideoTracks()[0];
            if (vtrack && vtrack.addEventListener) {
                vtrack.addEventListener('unmute', function () {
                    tryPlay();
                    applyVideoDisplaySize(session.activeVideo);
                    updateVideoSizeInfo(doc);
                });
            }
            session.activeVideo.onloadedmetadata = function () {
                if (session.activeVideo.videoWidth && session.activeVideo.videoHeight) {
                    session.rpcStreamW = session.activeVideo.videoWidth;
                    session.rpcStreamH = session.activeVideo.videoHeight;
                    /* Electron 紧凑模式往往未带 rpcWindow=1，也必须置位，否则 10s 无视频定时器会一直误判 */
                    if (session.rpcWindowMode || session.electronCompactLauncher) {
                        onRpcVideoStreamReady(session);
                    }
                }
                applyVideoDisplaySize(session.activeVideo);
                updateVideoSizeInfo(doc);
                setupRemoteControl(session, doc);
                const splash = doc.getElementById('rpc-window-wait');
                if (splash) splash.hidden = true;
            };
            session.activeVideo.onplay = function () { setupRemoteControl(session, doc); };
            session.activeVideo.onplaying = function () {
                if (session.rpcHadVideo || (!session.rpcWindowMode && !session.electronCompactLauncher)) return;
                if (session.activeVideo.videoWidth && session.activeVideo.videoHeight) {
                    session.rpcStreamW = session.activeVideo.videoWidth;
                    session.rpcStreamH = session.activeVideo.videoHeight;
                }
                onRpcVideoStreamReady(session);
            };
            /* 解码后分辨率变化时更新显示尺寸与鼠标映射 */
            session.activeVideo.addEventListener('resize', function () {
                if (!session.activeVideo) return;
                if (session.activeVideo.videoWidth && session.activeVideo.videoHeight) {
                    session.rpcStreamW = session.activeVideo.videoWidth;
                    session.rpcStreamH = session.activeVideo.videoHeight;
                }
                applyVideoDisplaySize(session.activeVideo);
                updateVideoSizeInfo(doc);
            });
            function tryPlay() {
                const p = session.activeVideo.play();
                if (p && typeof p.catch === 'function') {
                    p.catch(function (err) {
                        logDataChannel(ui, '视频播放: ' + (err && err.message ? err.message : err) + ' — 请点击画面');
                    });
                }
            }
            tryPlay();
            requestAnimationFrame(tryPlay);
            latencyDiag.bindVideo(session.activeVideo);
        }

        function createPeerConnection() {
            const config = { bundlePolicy: 'max-bundle' };
            const useStun = doc.getElementById('use-stun');
            if (useStun && useStun.checked) {
                config.iceServers = [{ urls: ['stun:stun.l.google.com:19302'] }];
            }
            const peer = new RTCPeerConnection(config);

            peer.addEventListener('iceconnectionstatechange', function () {
                if (ui.iceConnectionState) ui.iceConnectionState.textContent = peer.iceConnectionState;
                if (ui.iceConnectionIndicator) {
                    ui.iceConnectionIndicator.className =
                        'status-indicator ' + (peer.iceConnectionState === 'connected' ? 'active' : '');
                }
            });
            peer.addEventListener('icegatheringstatechange', function () {
                if (ui.iceGatheringState) ui.iceGatheringState.textContent = peer.iceGatheringState;
            });
            peer.addEventListener('signalingstatechange', function () {
                if (ui.signalingState) ui.signalingState.textContent = peer.signalingState;
            });

            peer.addEventListener('connectionstatechange', function () {
                const st = peer.connectionState;
                if ((!session.rpcWindowMode && !session.electronCompactLauncher) || session.rpcAutoClosed) return;
                if (st === 'failed') {
                    if (!shouldDeferRpcShellCloseUntilVideo(session)) {
                        closeRpcShellOrWindow(session, 'webrtc_connection_failed');
                    }
                    return;
                }
                if (st === 'disconnected') {
                    if (!session.rpcHadVideo) return;
                    clearSessionTimer(session, 'rpcPcDisconnectTimer');
                    session.rpcPcDisconnectTimer = setTimeout(function () {
                        session.rpcPcDisconnectTimer = null;
                        if (session.rpcAutoClosed) return;
                        if (session.pc && session.pc.connectionState === 'disconnected') {
                            closeRpcShellOrWindow(session, 'webrtc_disconnected_timeout');
                        }
                    }, 8000);
                    return;
                }
                if (st === 'connected' || st === 'connecting') {
                    clearSessionTimer(session, 'rpcPcDisconnectTimer');
                }
            });

            peer.ontrack = function (evt) {
                const t = evt.track;
                if (!t || t.kind !== 'video') {
                    return;
                }
                /* 切勿对 audio 的 ontrack 写 video.srcObject，否则会覆盖掉已有视频轨（常见 SDP 顺序：先 video 后 audio） */
                if (session.rpcWindowMode || session.electronCompactLauncher) {
                    t.addEventListener('ended', function () {
                        if (!shouldDeferRpcShellCloseUntilVideo(session)) {
                            exitVideoPageAfterRemoteStreamEnded(session, doc, ui, 'video_track_ended');
                        }
                    });
                }
                if (typeof t.getSettings === 'function') {
                    try {
                        const s = t.getSettings();
                        if (s.width) session.rpcStreamW = s.width;
                        if (s.height) session.rpcStreamH = s.height;
                    } catch (_) {}
                }
                /* 等待层 z-index 高于 video-stage，需在出画前隐藏，否则一直挡在黑底上 */
                if (session.rpcWindowMode) {
                    const splash = doc.getElementById('rpc-window-wait');
                    if (splash) splash.hidden = true;
                }
                showVideoStage(session, doc);
                const v = getMainVideo(doc);
                attachStreamToMainVideo(v, new MediaStream([t]));
            };

            peer.ondatachannel = function (evt) {
                session.dc = evt.channel;
                updateDataChannelState(ui, session.dc.readyState);
                session.dc.onopen = function () {
                    updateDataChannelState(ui, 'open');
                    logDataChannel(ui, 'Data channel opened');
                    latencyDiag.onDataChannelOpen();
                    try {
                        session.dc.send(JSON.stringify({ type: 'controlRequest' }));
                    } catch (_) {}
                };
                session.dc.onmessage = function (ev) {
                    if (typeof ev.data !== 'string') return;
                    const str = ev.data;
                    try {
                        const j0 = JSON.parse(str);
                        if (j0 && j0.type === 'remoteProcessExited') {
                            /* 服务端可能在主窗 HWND 瞬时失效时误发退出；轨仍 live 时忽略，改由 track ended 等关窗 */
                            if (rpcShellCloseAllowed(session) && session.pc) {
                                try {
                                    const recvs = session.pc.getReceivers();
                                    for (let i = 0; i < recvs.length; i++) {
                                        const tr = recvs[i] && recvs[i].track;
                                        if (tr && tr.kind === 'video' && tr.readyState === 'live') {
                                            logDataChannel(ui, '忽略 remoteProcessExited（视频轨仍为 live，可能为窗口重建误报）');
                                            return;
                                        }
                                    }
                                } catch (_) {}
                            }
                            exitVideoPageAfterRemoteStreamEnded(session, doc, ui, 'remote_process_exited');
                            return;
                        }
                    } catch (_) {}
                    if (!latencyDiag.tryHandleMessage(str)) {
                        logDataChannel(ui, '< ' + str);
                    }
                    try {
                        const j = JSON.parse(str);
                        if (j && j.type === 'controlGranted') session.isControlEnabled = true;
                        if (j && (j.type === 'controlDenied' || j.type === 'controlRevoked')) {
                            session.isControlEnabled = false;
                        }
                    } catch (_) {}
                    if (str.indexOf('Ping') !== -1) {
                        setTimeout(function () {
                            if (!session.dc) return;
                            const message = 'Pong ' + currentTimestamp();
                            logDataChannel(ui, '> ' + message);
                            session.dc.send(message);
                        }, 1000);
                    }
                };
                session.dc.onclose = function () {
                    updateDataChannelState(ui, 'closed');
                    logDataChannel(ui, 'Data channel closed');
                };
                session.dc.onerror = function (error) {
                    logDataChannel(ui, 'Data channel error: ' + error);
                };
            };

            return peer;
        }

        function waitGatheringComplete(peer) {
            return new Promise(function (resolve) {
                if (peer.iceGatheringState === 'complete') resolve();
                else {
                    peer.addEventListener('icegatheringstatechange', function handler() {
                        if (peer.iceGatheringState === 'complete') {
                            peer.removeEventListener('icegatheringstatechange', handler);
                            resolve();
                        }
                    });
                }
            });
        }

        function sendAnswer(peer) {
            return peer.createAnswer().then(function (answer) {
                return peer.setLocalDescription(answer);
            }).then(function () {
                return waitGatheringComplete(peer);
            }).then(function () {
                const answer = peer.localDescription;
                const answerEl = doc.getElementById('answer-sdp');
                if (answerEl) answerEl.textContent = answer.sdp;
                session.websocket.send(JSON.stringify({
                    id: 'server', type: answer.type, sdp: answer.sdp,
                }));
            });
        }

        function handleOffer(message) {
            session.pc = createPeerConnection();
            latencyDiag.bindPeer(session.pc);
            const desc = new RTCSessionDescription({ type: message.type, sdp: message.sdp });
            return session.pc.setRemoteDescription(desc).then(function () {
                return sendAnswer(session.pc);
            });
        }

        function reattachVideoToMain() {
            if (!session.pc) return;
            const v = getMainVideo(doc);
            const recv = session.pc.getReceivers().find(function (r) {
                return r.track && r.track.kind === 'video';
            });
            if (!recv || !recv.track || !v) return;
            attachStreamToMainVideo(v, new MediaStream([recv.track]));
        }

        return { handleOffer: handleOffer, reattachVideoToMain: reattachVideoToMain, attachStreamToMainVideo: attachStreamToMainVideo };
    }

    function createSignalingClient(session, doc, ui, callbacks) {
        const onOffer = callbacks.onOffer;
        function connect() {
            try {
                const wsUrl = buildSignalingWebSocketUrl(session.clientId);
                console.info('[RemoteProcessControl] 信令 WebSocket:', wsUrl);
                session.websocket = new WebSocket(wsUrl);
                updateWebSocketState(ui, 'connecting');

                session.websocket.onopen = function () {
                    clearSessionTimer(session, 'rpcWsConnectTimer');
                    updateWebSocketState(ui, 'connected');
                    const startBtn = doc.getElementById('start');
                    if (startBtn) startBtn.disabled = false;
                    logDataChannel(ui, 'WebSocket connected. clientId=' + session.clientId);
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
                            logDataChannel(ui, '处理 offer 失败: ' + (offerErr && offerErr.message ? offerErr.message : offerErr));
                        });
                    }
                };

                session.websocket.onerror = function () {
                    logDataChannel(ui, 'WebSocket 错误（请确认信令服务已监听 9090，且 HTTPS 页面需使用 WSS 或改为 HTTP 打开前端）');
                    updateWebSocketState(ui, 'error');
                    if (session.rpcWindowMode || session.electronCompactLauncher) {
                        window.setTimeout(function () {
                            if (!shouldDeferRpcShellCloseUntilVideo(session)) {
                                closeRpcShellOrWindow(session, 'websocket_error');
                            }
                        }, 500);
                    }
                };

                session.websocket.onclose = function (ev) {
                    logDataChannel(ui, 'WebSocket closed (code=' + ev.code + (ev.reason ? ', ' + ev.reason : '') + ')');
                    updateWebSocketState(ui, 'disconnected');
                    if (session.rpcWindowMode || session.electronCompactLauncher) {
                        window.setTimeout(function () {
                            if (!shouldDeferRpcShellCloseUntilVideo(session)) {
                                closeRpcShellOrWindow(session, 'websocket_closed_' + ev.code);
                            }
                        }, 400);
                    }
                };
            } catch (e) {
                logDataChannel(ui, 'Failed to init WebSocket: ' + e);
                updateWebSocketState(ui, 'error');
                console.error('[RemoteProcessControl] WebSocket 构造失败:', e);
            }
        }
        return { connect: connect };
    }

    function RemoteProcessApplication(doc) {
        this.doc = doc;
        this.session = createSession();
        this.ui = null;
        this.webrtc = null;
        this.signaling = null;
    }

    RemoteProcessApplication.prototype.sendRequest = function () {
        const exeEl = this.doc.getElementById('exe-path');
        const exePath = (exeEl && exeEl.value) ? exeEl.value.trim() : '';
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
        if (this.session.rpcWindowMode || this.session.electronCompactLauncher) {
            armRpcNoVideoWatchdog(this.session);
            if (this.session.electronCompactLauncher && !this.session.rpcAutostart) {
                armRpcWebSocketConnectWatchdog(this.session);
            }
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
        this.webrtc = createWebRtcSessionController(this.session, this.doc, this.ui);
        const self = this;
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
        this.bindDom();
        /* 紧凑模式且手动双击再连：勿在启动页就开始「信令 10s 未连上关窗」，改到 start() 再计时 */
        const deferWsWatchdog =
            this.session.electronCompactLauncher && !this.session.rpcAutostart;
        if ((this.session.rpcWindowMode || this.session.electronCompactLauncher) && !deferWsWatchdog) {
            armRpcWebSocketConnectWatchdog(this.session);
        }
        this.signaling.connect();
    };

    function startApp() {
        if (window.__rpcAppStarted) return;
        window.__rpcAppStarted = true;
        try {
            const app = new RemoteProcessApplication(document);
            applyRpcWindowUrlFlags(app.session, document);
            applyElectronShellFlags(app.session, document);
            window.__rpcApp = app;
            app.run();
        } catch (err) {
            console.error('[RemoteProcessControl] 启动失败:', err);
            const pre = document.getElementById('data-channel-log');
            if (pre) {
                pre.textContent += '\n[错误] 前端脚本无法启动: ' + (err && err.message ? err.message : err) + '\n';
            }
        }
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', startApp, { once: true });
    } else {
        startApp();
    }
})();
