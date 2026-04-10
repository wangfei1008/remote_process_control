/**
 * Video chain / session layer
 */
(function () {
    'use strict';
    window.__rpcWebRtc = window.__rpcWebRtc || {};

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

    function smoothEwma(prev, sample, alpha) {
        if (prev == null || isNaN(prev)) return sample;
        return prev * (1 - alpha) + sample * alpha;
    }

    /**
     * 延时诊断：DataChannel 对时(latPing/latPong)、frameMark、getStats(抖动缓冲/解码/ICE RTT)、requestVideoFrameCallback。
     * 总延时为粗算：发送端采集+编码 + 编码结束→DC收到 + 抖动缓冲均值 + 解码均值（详见控制台 [latency] 日志）。
     */
    window.__rpcWebRtc.createLatencyDiagnostics = function createLatencyDiagnostics(session, doc) {
        const state = {
            thetaMs: null,
            rttMs: null,
            lastDcLagMs: null,
            lastMarkCapMs: null,
            lastMarkEncMs: null,
            lastMarkSeq: null,
            lastCaptureHealthSummary: '',
            rvfHandle: null,
            pingTimer: null,
            statsTimer: null,
            videoEl: null,
            pingSeq: 0,
            pingCount: 0,
            lastFramesDropped: null,
            lastKeyframeRequestAt: 0,
            prevJbd: null,
            prevJbec: null,
            highJitterStreak: 0,
            severeJitterStreak: 0,
            highJitterSince: null,
            lastSoftResetAt: 0,
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
                    const alpha = (state.pingCount != null && state.pingCount <= 5) ? 0.6 : 0.15;
                    state.thetaMs = smoothEwma(state.thetaMs, theta, alpha);
                    state.rttMs = smoothEwma(state.rttMs, rtt, alpha);
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
                if (j.type === 'captureHealth') {
                    const backend = String(j.backend || 'unknown');
                    const dxgiDisabled = !!j.dxgiDisabledForSession;
                    const stripStreak = Number(j.topBlackStripStreak || 0);
                    const dxgiScore = Number(j.dxgiInstabilityScore || 0);
                    const forceSw = !!j.forceSoftwareActive;
                    const forceSwRemain = Number(j.forceSoftwareRemainMs || 0);
                    const capMs = Number(j.capMs || 0);
                    const encMs = Number(j.encMs || 0);
                    const summary = 'backend=' + backend
                        + ' score=' + dxgiScore
                        + (dxgiDisabled ? ' (dxgi_disabled)' : '')
                        + ' strip=' + stripStreak
                        + (forceSw ? (' sw_fallback=' + Math.max(0, Math.round(forceSwRemain)) + 'ms') : '')
                        + ' cap/enc=' + capMs + '/' + encMs + 'ms';
                    state.lastCaptureHealthSummary = summary;
                    if (forceSw || dxgiDisabled || stripStreak > 0) {
                        console.warn('[capture] health ' + summary);
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

        function scheduleLatPing() {
            if (!session.dc || session.dc.readyState !== 'open') return;
            if (state.pingCount == null) state.pingCount = 0;
            state.pingCount += 1;
            sendLatPing();
            const interval = state.pingCount < 5 ? 500 : 3000;
            state.pingTimer = setTimeout(scheduleLatPing, interval);
        }

        function requestKeyframe(reason, detail) {
            if (!session.dc || session.dc.readyState !== 'open') return;
            const now = Date.now();
            // Avoid spamming keyframe requests.
            if (state.lastKeyframeRequestAt && (now - state.lastKeyframeRequestAt) < 5000) return;
            state.lastKeyframeRequestAt = now;
            try {
                session.dc.send(JSON.stringify({
                    type: 'requestKeyframe',
                    reason: reason || 'latency',
                    detail: detail || null,
                }));
                console.info('[latency] requestKeyframe reason=' + (reason || 'latency'));
            } catch (_) {}
        }

        function softResetVideoSink(reason) {
            const now = Date.now();
            // Cooling down longer to avoid frequent srcObject rebind.
            if (state.lastSoftResetAt && (now - state.lastSoftResetAt) < 15000) return;
            if (!session.pc) return;
            const v = session.activeVideo || doc.getElementById('rpc-main-video');
            if (!v) return;
            try {
                const recv = session.pc.getReceivers && session.pc.getReceivers().find(function (r) {
                    return r && r.track && r.track.kind === 'video' && r.track.readyState === 'live';
                });
                if (!recv || !recv.track) return;
                // If the track is still live/enable/unmuted, avoid rebind (it causes visible black frames).
                const live = recv.track.readyState === 'live';
                const enabled = recv.track.enabled !== false;
                const muted = !!recv.track.muted;
                if (live && enabled && !muted) {
                    requestKeyframe(reason + '_fallback', null);
                    return;
                }
                // Only rebind if receiver track is actually abnormal (ended/disabled/muted).
                v.srcObject = new MediaStream([recv.track]);
                const p = v.play && v.play();
                if (p && typeof p.catch === 'function') p.catch(function () {});
                state.lastSoftResetAt = now;
                console.warn('[latency] softResetVideoSink reason=' + reason);
            } catch (_) {}
        }

        function applyPlayoutDelayHint(jitterBufMs) {
            if (!session.pc || !session.pc.getReceivers) return;
            let hint = 0.02; // default 20ms (seconds)
            if (typeof jitterBufMs === 'number' && !isNaN(jitterBufMs)) {
                if (jitterBufMs < 30) hint = 0.005;       // 5ms
                else if (jitterBufMs < 80) hint = 0.03;  // 30ms
                else if (jitterBufMs < 150) hint = 0.06; // 60ms
                else hint = 0.1;                         // 100ms
            }
            try {
                const receivers = session.pc.getReceivers();
                for (let k = 0; k < receivers.length; k++) {
                    const r = receivers[k];
                    if (!r || !r.track || r.track.kind !== 'video') continue;
                    if (!('playoutDelayHint' in r)) continue;
                    try { r.playoutDelayHint = hint; } catch (_) {}
                }
            } catch (_) {}
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
                let framesDroppedNow = null;
                if (inboundVideo) {
                    const jbd = inboundVideo.jitterBufferDelay;
                    const jbec = inboundVideo.jitterBufferEmittedCount;
                    // Prefer delta-window average instead of all-time cumulative average.
                    // This avoids "ever-growing" values when old backlog pollutes denominator.
                    if (typeof jbd === 'number' && typeof jbec === 'number') {
                        if (typeof state.prevJbd === 'number' && typeof state.prevJbec === 'number') {
                            const dDelay = jbd - state.prevJbd;
                            const dEmit = jbec - state.prevJbec;
                            if (dDelay >= 0 && dEmit > 0) {
                                jitterBufMs = (dDelay / dEmit) * 1000;
                            }
                        }
                        if (jitterBufMs == null && jbec > 0) {
                            jitterBufMs = (jbd / jbec) * 1000;
                        }
                        state.prevJbd = jbd;
                        state.prevJbec = jbec;
                    }
                    if (jitterBufMs != null) {
                        parts.push('抖动缓冲均值≈' + Math.round(jitterBufMs) + 'ms');
                        applyPlayoutDelayHint(jitterBufMs);
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
                        framesDroppedNow = inboundVideo.framesDropped;
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

                // Packet loss recovery:
                // When framesDropped jumps or jitterBufferDelay is too large, request a keyframe from server.
                if (framesDroppedNow != null) {
                    const lastFd = state.lastFramesDropped;
                    const deltaFd = (typeof lastFd === 'number') ? (framesDroppedNow - lastFd) : 0;
                    // 1) sustained high jitter -> keyframe
                    if (jitterBufMs != null && jitterBufMs >= 180) {
                        if (!state.highJitterSince) state.highJitterSince = Date.now();
                        else if (Date.now() - state.highJitterSince >= 4000) {
                            requestKeyframe('sustained_high_jitter', { jitterBufMs: jitterBufMs, framesDroppedNow: framesDroppedNow });
                            state.highJitterSince = null;
                        }
                    } else {
                        state.highJitterSince = null;
                    }

                    // 2) track severe jitter streak (>=600ms). Stats period is 2s, so streak>=5 ~= 10s.
                    if (jitterBufMs != null && jitterBufMs >= 600) state.severeJitterStreak += 1;
                    else state.severeJitterStreak = 0;

                    // 3) frame drop delta -> keyframe
                    if (deltaFd >= 3) {
                        requestKeyframe('packet_loss', {
                            framesDroppedDelta: deltaFd,
                            framesDroppedNow: framesDroppedNow,
                            jitterBufMs: jitterBufMs,
                        });
                    }

                    // 4) Only extremely bad conditions -> soft reset (but softResetVideoSink itself avoids rebind on live tracks).
                    if ((state.severeJitterStreak >= 5) || (deltaFd >= 15)) {
                        requestKeyframe('severe_jitter', {
                            framesDroppedDelta: deltaFd,
                            framesDroppedNow: framesDroppedNow,
                            jitterBufMs: jitterBufMs,
                        });
                        // Only rebind when receiver track is truly abnormal (ended/disabled/muted),
                        // otherwise prefer requestKeyframe-only to avoid visible black frames.
                        let needRebind = false;
                        try {
                            const recv = session.pc && session.pc.getReceivers && session.pc.getReceivers().find(function (r) {
                                return r && r.track && r.track.kind === 'video';
                            });
                            if (recv && recv.track) {
                                const tr = recv.track;
                                needRebind = (tr.readyState !== 'live') || (tr.enabled === false) || (!!tr.muted);
                            }
                        } catch (_) {}
                        if (needRebind) softResetVideoSink('severe_jitter');
                        state.severeJitterStreak = 0;
                    }
                    state.lastFramesDropped = framesDroppedNow;
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
                if (state.lastCaptureHealthSummary) shortHud.push(state.lastCaptureHealthSummary);
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
            requestKeyframe: requestKeyframe,
            onDataChannelOpen: function () {
                if (state.pingTimer) clearTimeout(state.pingTimer);
                state.pingTimer = null;
                state.pingCount = 0;
                scheduleLatPing();
                // Speed up "first picture" and recovery: ask sender for an IDR on startup.
                requestKeyframe('startup', { at: Date.now() });
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
                if (state.pingTimer) clearTimeout(state.pingTimer);
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
            getCaptureHealthSummary: function () {
                return state.lastCaptureHealthSummary || '';
            },
        };
    };

    window.__rpcWebRtc.createWebRtcSessionController = function createWebRtcSessionController(session, doc, ui) {
        const i = window.__rpcInternal || {};
        const currentTimestamp = createTimestampState();
        let captureHealthLastUi = '';

        const latencyDiag = (window.__rpcWebRtc && typeof window.__rpcWebRtc.createLatencyDiagnostics === 'function')
            ? window.__rpcWebRtc.createLatencyDiagnostics(session, doc)
            : {
                tryHandleMessage: function () { return false; },
                onDataChannelOpen: function () {},
                bindPeer: function () {},
                bindVideo: function () {},
                dispose: function () {},
                getCaptureHealthSummary: function () { return ''; },
            };
        session.latencyDiag = latencyDiag;

        function updateDataChannelState(ui, state) {
            if (!ui) return;
            if (ui.dataChannelState) ui.dataChannelState.textContent = state;
            if (ui.dataChannelIndicator) {
                ui.dataChannelIndicator.className = 'status-indicator ' + (state === 'open' ? 'active' : '');
            }
        }

        function updateCaptureHealthState(summary) {
            if (!ui || !ui.captureHealthState) return;
            ui.captureHealthState.textContent = summary || 'waiting...';
        }

        function getMainVideo(doc) {
            if (window.__rpcUI && typeof window.__rpcUI.getMainVideo === 'function') {
                return window.__rpcUI.getMainVideo(doc);
            }
            return doc.getElementById('rpc-main-video');
        }

        function updateVideoSizeInfo(doc) {
            if (window.__rpcUI && typeof window.__rpcUI.updateVideoSizeInfo === 'function') {
                window.__rpcUI.updateVideoSizeInfo(doc);
            }
        }

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
                    i.onRpcVideoStreamReady && i.onRpcVideoStreamReady(session);
                    tryPlay();
                    i.applyVideoDisplaySize && i.applyVideoDisplaySize(session.activeVideo);
                    updateVideoSizeInfo(doc);
                });
            }
            session.activeVideo.onloadedmetadata = function () {
                if (session.activeVideo.videoWidth && session.activeVideo.videoHeight) {
                    session.rpcStreamW = session.activeVideo.videoWidth;
                    session.rpcStreamH = session.activeVideo.videoHeight;
                    i.onRpcVideoStreamReady && i.onRpcVideoStreamReady(session);
                }
                i.applyVideoDisplaySize && i.applyVideoDisplaySize(session.activeVideo);
                updateVideoSizeInfo(doc);
                i.setupRemoteControl && i.setupRemoteControl(session, doc);
                const splash = doc.getElementById('rpc-window-wait');
                if (splash) splash.hidden = true;
            };
            session.activeVideo.onplay = function () { i.setupRemoteControl && i.setupRemoteControl(session, doc); };
            session.activeVideo.onplaying = function () {
                if (session.activeVideo.videoWidth && session.activeVideo.videoHeight) {
                    session.rpcStreamW = session.activeVideo.videoWidth;
                    session.rpcStreamH = session.activeVideo.videoHeight;
                }
                i.onRpcVideoStreamReady && i.onRpcVideoStreamReady(session);
            };

            /* 解码后分辨率变化时更新宽高/鼠标映射（显示尺寸固定为 100%） */
            session.activeVideo.addEventListener('resize', function () {
                if (!session.activeVideo) return;
                const v = session.activeVideo;
                if (v.videoWidth && v.videoHeight) {
                    session.rpcStreamW = v.videoWidth;
                    session.rpcStreamH = v.videoHeight;
                    i.onRpcVideoStreamReady && i.onRpcVideoStreamReady(session);
                }
                const now = Date.now();
                if (v.__rpc_last_resize_apply_ms != null && (now - v.__rpc_last_resize_apply_ms) < 200) return;
                v.__rpc_last_resize_apply_ms = now;
                i.applyVideoDisplaySize && i.applyVideoDisplaySize(v);
                updateVideoSizeInfo(doc);
            });
            if (typeof session.activeVideo.requestVideoFrameCallback === 'function') {
                const vv = session.activeVideo;
                vv.__rpc_last_frame_w = vv.videoWidth || 0;
                vv.__rpc_last_frame_h = vv.videoHeight || 0;
                const onVideoFrame = function () {
                    if (!session.activeVideo || session.activeVideo !== vv) return;
                    const wNow = vv.videoWidth || 0;
                    const hNow = vv.videoHeight || 0;
                    if (wNow > 0 && hNow > 0 &&
                        (wNow !== vv.__rpc_last_frame_w || hNow !== vv.__rpc_last_frame_h)) {
                        vv.__rpc_last_frame_w = wNow;
                        vv.__rpc_last_frame_h = hNow;
                        session.rpcStreamW = wNow;
                        session.rpcStreamH = hNow;
                        i.applyVideoDisplaySize && i.applyVideoDisplaySize(vv);
                        updateVideoSizeInfo(doc);
                    }
                    i.onRpcVideoStreamReady && i.onRpcVideoStreamReady(session);
                    vv.requestVideoFrameCallback(onVideoFrame);
                };
                vv.requestVideoFrameCallback(onVideoFrame);
            }
            function tryPlay() {
                const p = session.activeVideo.play();
                if (p && typeof p.catch === 'function') {
                    p.catch(function (err) {
                        i.logDataChannel && i.logDataChannel(ui, '视频播放: ' + (err && err.message ? err.message : err) + ' — 请点击画面');
                    });
                }
            }
            tryPlay();
            requestAnimationFrame(tryPlay);
            latencyDiag.bindVideo(session.activeVideo);
        }

        function createPeerConnection() {
            const config = { bundlePolicy: 'max-bundle', iceCandidatePoolSize: 4 };
            const useStun = doc.getElementById('use-stun');
            if (useStun && useStun.checked) {
                config.iceServers = [{ urls: ['stun:stun.l.google.com:19302'] }];
            }
            const peer = new RTCPeerConnection(config);

            function nowTs() {
                try { return new Date().toISOString(); } catch (_) { return String(Date.now()); }
            }
            function logPc(tag) {
                console.info('[rpc] pc:' + tag
                    + ' ts=' + nowTs()
                    + ' clientId=' + session.clientId
                    + ' signaling=' + peer.signalingState
                    + ' ice=' + peer.iceConnectionState
                    + ' conn=' + peer.connectionState);
            }

            peer.addEventListener('iceconnectionstatechange', function () {
                if (ui.iceConnectionState) ui.iceConnectionState.textContent = peer.iceConnectionState;
                if (ui.iceConnectionIndicator) {
                    ui.iceConnectionIndicator.className =
                        'status-indicator ' + (peer.iceConnectionState === 'connected' ? 'active' : '');
                }
                logPc('iceconnectionstatechange');
            });
            peer.addEventListener('icegatheringstatechange', function () {
                if (ui.iceGatheringState) ui.iceGatheringState.textContent = peer.iceGatheringState;
                logPc('icegatheringstatechange');
            });
            peer.addEventListener('signalingstatechange', function () {
                if (ui.signalingState) ui.signalingState.textContent = peer.signalingState;
                logPc('signalingstatechange');
            });

            peer.addEventListener('connectionstatechange', function () {
                const st = peer.connectionState;
                logPc('connectionstatechange');
                if (!session.rpcWindowMode || session.rpcAutoClosed) return;
                if (st === 'failed') {
                    if (!i.shouldDeferRpcShellCloseUntilVideo || !i.shouldDeferRpcShellCloseUntilVideo(session)) {
                        i.closeRpcShellOrWindow && i.closeRpcShellOrWindow(session, 'webrtc_connection_failed');
                    }
                    return;
                }
                if (st === 'disconnected') {
                    if (!session.rpcHadVideo) return;
                    i.clearSessionTimer && i.clearSessionTimer(session, 'rpcPcDisconnectTimer');
                    session.rpcPcDisconnectTimer = setTimeout(function () {
                        session.rpcPcDisconnectTimer = null;
                        if (session.rpcAutoClosed) return;
                        if (session.pc && session.pc.connectionState === 'disconnected') {
                            i.closeRpcShellOrWindow && i.closeRpcShellOrWindow(session, 'webrtc_disconnected_timeout');
                        }
                    }, 8000);
                    return;
                }
                if (st === 'connected' || st === 'connecting') {
                    i.clearSessionTimer && i.clearSessionTimer(session, 'rpcPcDisconnectTimer');
                }
            });

            peer.ontrack = function (evt) {
                const t = evt.track;
                if (!t || t.kind !== 'video') {
                    return;
                }
                // Reduce receiver playout buffer when supported (best-effort).
                // Long jitter buffers + decode drops often manifest as visible flicker/black.
                try {
                    // Some Chromium builds expose the property but default value can be undefined.
                    // Use 'in' check + try/catch to ensure we actually write the hint.
                    if (evt.receiver && ('playoutDelayHint' in evt.receiver)) {
                        // Unit is seconds, not milliseconds.
                        // Start from conservative 20ms; diagnostics will adjust dynamically.
                        evt.receiver.playoutDelayHint = 0.02; // 20ms (best-effort)
                    }
                } catch (_) {}
                /* 切勿对 audio 的 ontrack 写 video.srcObject，否则会覆盖掉已有视频轨（常见 SDP 顺序：先 video 后 audio） */
                if (session.rpcWindowMode) {
                    t.addEventListener('ended', function () {
                        i.exitVideoPageAfterRemoteStreamEnded && i.exitVideoPageAfterRemoteStreamEnded(session, doc, ui, 'video_track_ended');
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
                i.showVideoStage && i.showVideoStage(session, doc);
                const v = getMainVideo(doc);
                // Avoid resetting srcObject if the same track is already bound.
                try {
                    const curTrack = (session.activeVideo && session.activeVideo.srcObject &&
                        session.activeVideo.srcObject.getVideoTracks && session.activeVideo.srcObject.getVideoTracks()[0]) || null;
                    if (curTrack === t) {
                        latencyDiag.bindVideo && latencyDiag.bindVideo(session.activeVideo);
                        return;
                    }
                } catch (_) {}
                attachStreamToMainVideo(v, new MediaStream([t]));
                // Ask sender for an IDR right after first video track arrives.
                if (latencyDiag && typeof latencyDiag.requestKeyframe === 'function') {
                    latencyDiag.requestKeyframe('ontrack', { kind: 'video' });
                }

                // After receivers/transceivers settle, re-apply hint to the actual video receiver.
                // This reduces the chance that the initial receiver reference can't be updated yet.
                try {
                    setTimeout(function () {
                        if (!session.pc || !session.pc.getReceivers) return;
                        const receivers = session.pc.getReceivers();
                        for (let k = 0; k < receivers.length; k++) {
                            const r = receivers[k];
                            if (!r || !r.track || r.track.kind !== 'video') continue;
                            if ('playoutDelayHint' in r) {
                                try { r.playoutDelayHint = 0.02; } catch (_) {}
                            }
                        }
                    }, 0);
                } catch (_) {}
            };

            peer.ondatachannel = function (evt) {
                session.dc = evt.channel;
                updateDataChannelState(ui, session.dc.readyState);
                session.dc.onopen = function () {
                    updateDataChannelState(ui, 'open');
                    i.logDataChannel && i.logDataChannel(ui, 'Data channel opened');
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
                        if (j0 && j0.type === 'videoResolution') {
                            const w = Number(j0.width || 0);
                            const h = Number(j0.height || 0);
                            if (w > 0 && h > 0) {
                                const nowMs = Date.now();
                                const prevW = Number(session.__rpc_last_forced_w || 0);
                                const prevH = Number(session.__rpc_last_forced_h || 0);
                                const prevTs = Number(session.__rpc_last_forced_ts || 0);
                                const prevArea = (prevW > 0 && prevH > 0) ? (prevW * prevH) : 0;
                                const curArea = w * h;
                                const suspiciousTinyDrop = prevArea > 0 &&
                                    curArea < (prevArea * 0.2) &&
                                    (Math.min(w, h) < 120);
                                if (suspiciousTinyDrop && (nowMs - prevTs) < 8000) {
                                    console.warn('[rpc-res][dc] ignore suspicious tiny downgrade '
                                        + prevW + 'x' + prevH + ' -> ' + w + 'x' + h);
                                    return;
                                }
                                console.info('[rpc-res][dc] videoResolution from backend=' + w + 'x' + h);
                                session.rpcStreamW = w;
                                session.rpcStreamH = h;
                                session.__rpc_last_forced_w = w;
                                session.__rpc_last_forced_h = h;
                                session.__rpc_last_forced_ts = nowMs;
                                const vv = getMainVideo(doc);
                                if (vv) {
                                    // 后端分辨率作为显式信号，避免浏览器 resize 事件丢失时 UI 不更新。
                                    vv.__rpc_forced_w = w;
                                    vv.__rpc_forced_h = h;
                                    i.applyVideoDisplaySize && i.applyVideoDisplaySize(vv);
                                    console.info('[rpc-res][dc] video element now='
                                        + Number(vv.videoWidth || 0) + 'x' + Number(vv.videoHeight || 0)
                                        + ' forced=' + Number(vv.__rpc_forced_w || 0) + 'x' + Number(vv.__rpc_forced_h || 0));
                                }
                                updateVideoSizeInfo(doc);
                            }
                            return;
                        }
                        if (j0 && j0.type === 'remoteProcessExited') {
                            // 服务端在真实进程退出时已先发此消息再停采集；WebRTC 视频轨常短暂仍为 live，
                            // 不能以此忽略，否则 Electron/桌面壳子等不会立即关视频页。
                            i.logDataChannel && i.logDataChannel(ui, 'remoteProcessExited：关闭视频页');
                            i.exitVideoPageAfterRemoteStreamEnded && i.exitVideoPageAfterRemoteStreamEnded(session, doc, ui, 'remote_process_exited');
                            return;
                        }
                        if (j0 && j0.type === 'remoteWindowMissing') {
                            // 进程仍在，但当前无可采集窗口（启动/切换/遮挡/虚拟桌面等都可能导致短暂无 surfaces）。
                            // 只提示，不关闭视频页；等待窗口恢复后即可继续出画。
                            const why = String(j0.why || '');
                            const ms = Number(j0.missingMs || 0);
                            i.logDataChannel && i.logDataChannel(ui, 'remoteWindowMissing：等待窗口恢复'
                                + (ms > 0 ? (' missingMs=' + ms) : '')
                                + (why ? (' why=' + why) : ''));
                            return;
                        }
                    } catch (_) {}
                    if (!latencyDiag.tryHandleMessage(str)) {
                        i.logDataChannel && i.logDataChannel(ui, '< ' + str);
                    }
                    const capSummary = latencyDiag.getCaptureHealthSummary ? latencyDiag.getCaptureHealthSummary() : '';
                    if (capSummary && capSummary !== captureHealthLastUi) {
                        captureHealthLastUi = capSummary;
                        updateCaptureHealthState(capSummary);
                    }
                    try {
                        const j = JSON.parse(str);
                        if (j && j.type === 'controlGranted') session.isControlEnabled = true;
                        if (j && (j.type === 'controlDenied' || j.type === 'controlRevoked')) {
                            session.isControlEnabled = false;
                        }
                    } catch (_) {}
                    // Only respond to the server's plain-text keepalive "Ping".
                    // Do NOT match JSON messages like "latPing" (which contains substring "Ping").
                    if (str === 'Ping') {
                        setTimeout(function () {
                            if (!session.dc) return;
                            const message = 'Pong ' + currentTimestamp();
                            i.logDataChannel && i.logDataChannel(ui, '> ' + message);
                            session.dc.send(message);
                        }, 1000);
                    }
                };
                session.dc.onclose = function () {
                    updateDataChannelState(ui, 'closed');
                    captureHealthLastUi = '';
                    updateCaptureHealthState('disconnected');
                    i.logDataChannel && i.logDataChannel(ui, 'Data channel closed');
                };
                session.dc.onerror = function (error) {
                    i.logDataChannel && i.logDataChannel(ui, 'Data channel error: ' + error);
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
                session.websocket.send(JSON.stringify({
                    id: (session && session.rpcWorkNode) ? session.rpcWorkNode : 'server', type: answer.type, sdp: answer.sdp,
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
    };
})();

