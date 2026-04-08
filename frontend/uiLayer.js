/**
 * UI / 交互层
 * - 负责视频展示、全屏/退出
 * - 负责鼠标/键盘到远端坐标/按键映射
 * file:// 双击模式：普通 script，不使用 ES Module
 */
(function () {
    'use strict';

    window.__rpcUI = window.__rpcUI || {};

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

    function getMainVideo(doc) {
        return doc.getElementById('rpc-main-video');
    }

    function getVideoStage(doc) {
        return doc.getElementById('video-stage');
    }

    function resolveEffectiveVideoResolution(video, fallbackW, fallbackH) {
        if (!video) return { w: 0, h: 0 };
        const vw = Number(video.videoWidth || 0);
        const vh = Number(video.videoHeight || 0);
        const fw = Number(video.__rpc_forced_w || 0);
        const fh = Number(video.__rpc_forced_h || 0);
        const sw = Number(fallbackW || 0);
        const sh = Number(fallbackH || 0);
        const vArea = (vw > 0 && vh > 0) ? (vw * vh) : 0;
        const fArea = (fw > 0 && fh > 0) ? (fw * fh) : 0;

        // 后端通过 DataChannel 主动上报分辨率变化时，优先使用该值，
        // 避免浏览器 videoWidth/videoHeight 仍停留旧值导致 UI 尺寸不刷新。
        // 但如果浏览器已经解码出更大的稳定分辨率，且 forced 显著更小（常见于启动条/工具窗瞬时误报），
        // 则优先使用 videoWidth/videoHeight，避免 UI 被瞬时降级回小分辨率。
        if (vArea > 0 && fArea > 0 && vArea >= (fArea * 2)) {
            return { w: vw, h: vh };
        }
        if (fw > 0 && fh > 0) return { w: fw, h: fh };
        if (vw > 0 && vh > 0) return { w: vw, h: vh };
        if (sw > 0 && sh > 0) return { w: sw, h: sh };
        return { w: 0, h: 0 };
    }

    function logResolutionDecision(video, tag, payload) {
        if (!video) return;
        const now = Date.now();
        const key = tag + '|' + String(payload && payload.w || 0) + 'x' + String(payload && payload.h || 0)
            + '|' + String(payload && payload.fw || 0) + 'x' + String(payload && payload.fh || 0)
            + '|' + String(payload && payload.vw || 0) + 'x' + String(payload && payload.vh || 0);
        if (video.__rpc_last_res_log_key === key && (now - (video.__rpc_last_res_log_ms || 0)) < 1200) return;
        video.__rpc_last_res_log_key = key;
        video.__rpc_last_res_log_ms = now;
        try {
            console.info('[rpc-res][' + tag + '] effective=' + (payload.w || 0) + 'x' + (payload.h || 0)
                + ' forced=' + (payload.fw || 0) + 'x' + (payload.fh || 0)
                + ' video=' + (payload.vw || 0) + 'x' + (payload.vh || 0));
        } catch (_) {}
    }

    function pointerToVideoPixels(video, clientX, clientY, streamW, streamH) {
        const rect = video.getBoundingClientRect();
        const eff = resolveEffectiveVideoResolution(video, streamW, streamH);
        let iw = eff.w;
        let ih = eff.h;
        if (!iw || !ih) return null;
        const rw = rect.width;
        const rh = rect.height;
        let fitMode = 'contain';
        try {
            const cs = window.getComputedStyle ? window.getComputedStyle(video) : null;
            if (cs && cs.objectFit) fitMode = String(cs.objectFit).toLowerCase();
        } catch (_) {}
        const scale = (fitMode === 'cover') ? Math.max(rw / iw, rh / ih) : Math.min(rw / iw, rh / ih);
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

    function applyVideoDisplaySize(video) {
        if (!video) return;
        const eff = resolveEffectiveVideoResolution(video, 0, 0);
        const iw = eff.w;
        const ih = eff.h;
        if (!iw || !ih) return;
        logResolutionDecision(video, 'apply', {
            w: iw, h: ih,
            fw: Number(video.__rpc_forced_w || 0), fh: Number(video.__rpc_forced_h || 0),
            vw: Number(video.videoWidth || 0), vh: Number(video.videoHeight || 0),
        });
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
        // Fix video element size to reduce layout thrashing.
        // We let CSS `object-fit: contain` handle letterboxing stably.
        // Pointer mapping still uses actual boundingClientRect size.
        if (!video.__rpc_fill_fixed) {
            video.style.width = '100%';
            video.style.height = '100%';
            video.__rpc_fill_fixed = true;
        }
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
            notifyParentVideoResolution(doc, session.activeVideo);
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
        if (el && v) {
            const eff = resolveEffectiveVideoResolution(v, 0, 0);
            const w = eff.w;
            const h = eff.h;
            el.textContent = w + ' x ' + h;
            logResolutionDecision(v, 'hud', {
                w: w, h: h,
                fw: Number(v.__rpc_forced_w || 0), fh: Number(v.__rpc_forced_h || 0),
                vw: Number(v.videoWidth || 0), vh: Number(v.videoHeight || 0),
            });
        }
        notifyParentVideoResolution(doc, v);
    }

    function notifyParentVideoResolution(doc, videoEl) {
        if (!doc || !videoEl) return;
        const eff = resolveEffectiveVideoResolution(videoEl, 0, 0);
        const w = eff.w;
        const h = eff.h;
        if (!w || !h) return;
        try {
            if (window.parent && window.parent !== window && typeof window.parent.postMessage === 'function') {
                logResolutionDecision(videoEl, 'postMessage', {
                    w: w, h: h,
                    fw: Number(videoEl.__rpc_forced_w || 0), fh: Number(videoEl.__rpc_forced_h || 0),
                    vw: Number(videoEl.videoWidth || 0), vh: Number(videoEl.videoHeight || 0),
                });
                window.parent.postMessage({
                    type: 'rpc_video_resolution',
                    width: w,
                    height: h,
                }, '*');
            }
        } catch (_) {}
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

    // Expose
    window.__rpcUI.keyboardEventToWindowsVk = keyboardEventToWindowsVk;
    window.__rpcUI.pointerToVideoPixels = pointerToVideoPixels;
    window.__rpcUI.getMainVideo = getMainVideo;
    window.__rpcUI.getVideoStage = getVideoStage;
    window.__rpcUI.applyVideoDisplaySize = applyVideoDisplaySize;
    window.__rpcUI.tryEnterStageFullscreen = tryEnterStageFullscreen;
    window.__rpcUI.exitDocumentFullscreen = exitDocumentFullscreen;
    window.__rpcUI.showVideoStage = showVideoStage;
    window.__rpcUI.hideVideoStage = hideVideoStage;
    window.__rpcUI.updateVideoSizeInfo = updateVideoSizeInfo;
    window.__rpcUI.canSendControl = canSendControl;
    window.__rpcUI.isEventOnVideoHud = isEventOnVideoHud;
    window.__rpcUI.sendMouseMoveFromEvent = sendMouseMoveFromEvent;
    window.__rpcUI.setupStageMouse = setupStageMouse;
    window.__rpcUI.setupKeyboardOnStage = setupKeyboardOnStage;
    window.__rpcUI.setupRemoteControl = setupRemoteControl;

    // Backward compatibility for bindLayerExports().
    window.__rpcUI._bindFromInternal = window.__rpcUI._bindFromInternal || function () {};
})();

