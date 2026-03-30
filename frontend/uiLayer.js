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

