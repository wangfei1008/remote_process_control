/**
 * Desktop Container Mode
 * - 支持 file:// 加载：使用普通 script（非 ES Module）
 * - 暴露全局：window.__rpcStartDesktopMode(doc)
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

    window.__rpcStartDesktopMode = function __rpcStartDesktopMode(doc) {
        if (!doc) doc = document;
        if (doc.__rpcDesktopMounted) return;
        doc.__rpcDesktopMounted = true;

        // MVP: multi-window desktop container
        // Each remote "app window" is rendered by an iframe running rpcWindow=1.
        doc.documentElement.classList.add('rpc-desktop-mode');

        // Hide legacy single-session UI.
        const legacyContainer = doc.querySelector('.container');
        if (legacyContainer) legacyContainer.style.display = 'none';

        // Background root.
        const desktopRoot = doc.createElement('div');
        desktopRoot.id = 'desktop-root';
        desktopRoot.style.position = 'fixed';
        desktopRoot.style.inset = '0';
        desktopRoot.style.background = 'linear-gradient(135deg, #0b1220 0%, #101a2c 50%, #0b1220 100%)';
        desktopRoot.style.overflow = 'hidden';
        desktopRoot.style.userSelect = 'none';

        const windowLayer = doc.createElement('div');
        windowLayer.id = 'window-layer';
        windowLayer.style.position = 'absolute';
        windowLayer.style.inset = '0 0 48px 0';
        windowLayer.style.background = 'transparent';

        // Desktop icons layer (like OS desktop).
        const desktopIconsLayer = doc.createElement('div');
        desktopIconsLayer.id = 'desktop-icons-layer';
        desktopIconsLayer.style.position = 'absolute';
        desktopIconsLayer.style.left = '0';
        desktopIconsLayer.style.top = '0';
        desktopIconsLayer.style.right = '0';
        desktopIconsLayer.style.bottom = '48px';
        desktopIconsLayer.style.background = 'transparent';
        desktopIconsLayer.style.userSelect = 'none';
        desktopIconsLayer.style.zIndex = '1';

        const taskbar = doc.createElement('div');
        taskbar.id = 'desktop-taskbar';
        taskbar.style.position = 'absolute';
        taskbar.style.left = '0';
        taskbar.style.right = '0';
        taskbar.style.bottom = '0';
        taskbar.style.height = '48px';
        taskbar.style.background = 'rgba(10, 14, 22, 0.92)';
        taskbar.style.borderTop = '1px solid rgba(255,255,255,0.06)';
        taskbar.style.display = 'flex';
        taskbar.style.alignItems = 'center';
        taskbar.style.padding = '0 10px';
        taskbar.style.gap = '10px';
        taskbar.style.overflow = 'hidden';

        const startBtn = doc.createElement('button');
        startBtn.textContent = '开始';
        startBtn.style.height = '34px';
        startBtn.style.padding = '0 12px';
        startBtn.style.borderRadius = '8px';
        startBtn.style.border = '1px solid rgba(255,255,255,0.10)';
        startBtn.style.background = '#1a2233';
        startBtn.style.color = '#e7eefc';
        startBtn.style.cursor = 'pointer';
        startBtn.style.fontWeight = '600';

        const appsStrip = doc.createElement('div');
        appsStrip.style.display = 'flex';
        appsStrip.style.alignItems = 'center';
        appsStrip.style.gap = '8px';
        appsStrip.style.flex = '1';
        appsStrip.style.overflow = 'auto';
        appsStrip.style.paddingRight = '8px';

        const startMenu = doc.createElement('div');
        startMenu.id = 'desktop-start-menu';
        startMenu.style.position = 'absolute';
        startMenu.style.left = '12px';
        startMenu.style.bottom = '56px';
        startMenu.style.width = '360px';
        startMenu.style.maxHeight = '60vh';
        startMenu.style.overflow = 'auto';
        startMenu.style.background = 'rgba(14, 20, 32, 0.98)';
        startMenu.style.border = '1px solid rgba(255,255,255,0.08)';
        startMenu.style.borderRadius = '14px';
        startMenu.style.boxShadow = '0 16px 50px rgba(0,0,0,0.45)';
        startMenu.style.padding = '12px';
        startMenu.style.display = 'none';

        taskbar.appendChild(startBtn);
        taskbar.appendChild(appsStrip);

        const clockEl = doc.createElement('div');
        clockEl.style.flex = '0 0 auto';
        clockEl.style.color = 'rgba(231,238,252,0.95)';
        clockEl.style.fontWeight = '700';
        clockEl.style.fontSize = '12px';
        clockEl.style.letterSpacing = '0.3px';
        clockEl.style.marginLeft = 'auto';
        taskbar.appendChild(clockEl);

        desktopRoot.appendChild(windowLayer);
        desktopRoot.appendChild(desktopIconsLayer);
        desktopRoot.appendChild(taskbar);
        desktopRoot.appendChild(startMenu);
        doc.body.appendChild(desktopRoot);

        function updateClock() {
            try {
                clockEl.textContent = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
            } catch (_) {
                clockEl.textContent = '';
            }
        }
        updateClock();
        window.setInterval(updateClock, 1000);

        function toggleStartMenu(forceClose) {
            if (forceClose) {
                startMenu.style.display = 'none';
                return;
            }
            startMenu.style.display = (startMenu.style.display === 'none') ? 'block' : 'none';
        }
        startBtn.addEventListener('click', function (e) {
            e.stopPropagation();
            toggleStartMenu();
        });
        desktopRoot.addEventListener('mousedown', function (e) {
            if (e.target === startBtn || startMenu.contains(e.target)) return;
            toggleStartMenu(true);
        });

        // Read app shortcuts (from existing tiles).
        const appTileEls = Array.from(doc.querySelectorAll('.app-launch-tile[data-rpc-exe]'));
        let appDefs = appTileEls.map(function (el) {
            const exePath = el.getAttribute('data-rpc-exe') || '';
            const labelEl = el.querySelector('.tile-label');
            const iconEl = el.querySelector('.tile-icon');
            return {
                exePath: exePath,
                name: labelEl ? labelEl.textContent.trim() : exePath,
                icon: iconEl ? iconEl.textContent.trim() : '',
            };
        }).filter(function (a) { return a.exePath; });
        if (!appDefs.length) {
            appDefs = [{
                exePath: 'C:\\Windows\\System32\\notepad.exe',
                name: '记事本',
                icon: '📝',
            }];
        }
        appDefs.unshift({
            kind: 'my_data',
            name: '我的数据',
            icon: '📁',
            launchPath: (function () {
                const qs = new URLSearchParams(window.location.search);
                const signaling = qs.get('signaling');
                const p = new URLSearchParams();
                if (signaling) p.set('signaling', signaling);
                const suffix = p.toString();
                return './my_data.html' + (suffix ? ('?' + suffix) : '');
            })(),
        });

        function makeIframeSrc(exePath) {
            const qs = new URLSearchParams(window.location.search);
            qs.set('rpcWindow', '1');
            qs.set('autostart', '1');
            qs.set('exePath', exePath);
            return window.location.pathname + '?' + qs.toString();
        }

        // Desktop icons (列优先：先排一列，不行再排第二列)
        desktopIconsLayer.innerHTML = '';
        const iconCellW = 96;
        const iconCellH = 92;
        const iconStartX = 14;
        const iconStartY = 18;
        const desktopH = (desktopIconsLayer && desktopIconsLayer.clientHeight)
            ? desktopIconsLayer.clientHeight
            : (window.innerHeight - 48);
        const iconsPerCol = Math.max(1, Math.floor((desktopH - iconStartY) / iconCellH));

        appDefs.forEach(function (app, idx) {
            const col = Math.floor(idx / iconsPerCol);
            const row = idx % iconsPerCol;

            const iconWrap = doc.createElement('div');
            iconWrap.className = 'desktop-icon';
            iconWrap.style.position = 'absolute';
            iconWrap.style.left = String(iconStartX + col * iconCellW) + 'px';
            iconWrap.style.top = String(iconStartY + row * iconCellH) + 'px';
            iconWrap.style.width = '84px';
            iconWrap.style.height = '80px';
            iconWrap.style.display = 'flex';
            iconWrap.style.flexDirection = 'column';
            iconWrap.style.alignItems = 'center';
            iconWrap.style.justifyContent = 'flex-start';
            iconWrap.style.cursor = 'default';
            iconWrap.style.pointerEvents = 'auto';

            const iconBox = doc.createElement('div');
            iconBox.textContent = app.icon || '⬚';
            iconBox.style.width = '52px';
            iconBox.style.height = '52px';
            iconBox.style.borderRadius = '14px';
            iconBox.style.display = 'flex';
            iconBox.style.alignItems = 'center';
            iconBox.style.justifyContent = 'center';
            iconBox.style.fontSize = '28px';
            iconBox.style.background = 'rgba(255,255,255,0.06)';
            iconBox.style.border = '1px solid rgba(255,255,255,0.10)';

            const label = doc.createElement('div');
            label.textContent = app.name;
            label.style.marginTop = '6px';
            label.style.maxWidth = '84px';
            label.style.whiteSpace = 'nowrap';
            label.style.overflow = 'hidden';
            label.style.textOverflow = 'ellipsis';
            label.style.fontSize = '12px';
            label.style.fontWeight = '600';
            label.style.color = '#e7eefc';
            label.style.textShadow = '0 1px 2px rgba(0,0,0,0.35)';

            iconWrap.appendChild(iconBox);
            iconWrap.appendChild(label);
            iconWrap.title = app.exePath || app.name;

            iconWrap.addEventListener('dblclick', function (e) {
                e.stopPropagation();
                openAppWindow(app);
            });
            iconWrap.addEventListener('click', function (e) { e.stopPropagation(); });

            desktopIconsLayer.appendChild(iconWrap);
        });

        // Start menu apps.
        const menuSectionTitle = doc.createElement('div');
        menuSectionTitle.textContent = '应用程序';
        menuSectionTitle.style.color = 'rgba(231,238,252,0.85)';
        menuSectionTitle.style.fontSize = '11px';
        menuSectionTitle.style.fontWeight = '700';
        menuSectionTitle.style.letterSpacing = '0.06em';
        menuSectionTitle.style.margin = '4px 2px 10px';
        startMenu.appendChild(menuSectionTitle);

        const menuGrid = doc.createElement('div');
        menuGrid.style.display = 'flex';
        menuGrid.style.flexWrap = 'wrap';
        menuGrid.style.gap = '12px';
        startMenu.appendChild(menuGrid);

        appDefs.forEach(function (app) {
            const btn = doc.createElement('div');
            btn.style.width = '92px';
            btn.style.height = '92px';
            btn.style.borderRadius = '18px';
            btn.style.background = 'linear-gradient(145deg, #ffffff, #eef2ff)';
            btn.style.border = '2px solid #d8def5';
            btn.style.cursor = 'pointer';
            btn.style.display = 'flex';
            btn.style.flexDirection = 'column';
            btn.style.alignItems = 'center';
            btn.style.justifyContent = 'center';
            btn.style.boxShadow = '0 4px 12px rgba(110,142,251,0.15)';
            btn.title = app.exePath || app.name;

            const ic = doc.createElement('div');
            ic.textContent = app.icon || '⬚';
            ic.style.fontSize = '36px';
            ic.style.lineHeight = '1';
            btn.appendChild(ic);

            const lb = doc.createElement('div');
            lb.textContent = app.name;
            lb.style.fontSize = '12px';
            lb.style.fontWeight = '600';
            lb.style.marginTop = '8px';
            lb.style.color = '#4a5568';
            lb.style.textAlign = 'center';
            btn.appendChild(lb);

            btn.addEventListener('click', function (e) {
                e.stopPropagation();
                toggleStartMenu(true);
                openAppWindow(app);
            });
            menuGrid.appendChild(btn);
        });

        const menuFooter = doc.createElement('div');
        menuFooter.style.marginTop = '14px';
        menuFooter.style.paddingTop = '12px';
        menuFooter.style.borderTop = '1px solid rgba(255,255,255,0.08)';
        menuFooter.style.display = 'flex';
        menuFooter.style.gap = '10px';
        menuFooter.style.flexWrap = 'wrap';

        const btnSettings = doc.createElement('button');
        btnSettings.type = 'button';
        btnSettings.textContent = '系统设置（经典连接面板）';
        btnSettings.style.flex = '1 1 140px';
        btnSettings.style.minHeight = '36px';
        btnSettings.style.borderRadius = '10px';
        btnSettings.style.border = '1px solid rgba(255,255,255,0.10)';
        btnSettings.style.background = '#1a2233';
        btnSettings.style.color = '#e7eefc';
        btnSettings.style.cursor = 'pointer';
        btnSettings.style.fontWeight = '600';
        btnSettings.style.fontSize = '12px';

        const btnLogout = doc.createElement('button');
        btnLogout.type = 'button';
        btnLogout.textContent = '注销';
        btnLogout.style.flex = '1 1 120px';
        btnLogout.style.minHeight = '36px';
        btnLogout.style.borderRadius = '10px';
        btnLogout.style.border = '1px solid rgba(255,80,80,0.35)';
        btnLogout.style.background = 'rgba(60,20,20,0.9)';
        btnLogout.style.color = '#ffd6d6';
        btnLogout.style.cursor = 'pointer';
        btnLogout.style.fontWeight = '600';
        btnLogout.style.fontSize = '12px';

        menuFooter.appendChild(btnSettings);
        menuFooter.appendChild(btnLogout);
        startMenu.appendChild(menuFooter);

        btnSettings.addEventListener('click', function (e) {
            e.stopPropagation();
            const qs = new URLSearchParams(window.location.search);
            qs.delete('rpcWindow');
            qs.delete('kiosk');
            qs.delete('autostart');
            qs.delete('exePath');
            qs.set('rpcLegacy', '1');
            window.location.href = window.location.pathname + '?' + qs.toString();
        });

        // Desktop windows
        let zCounter = 10;
        const maxWindows = 5;
        const windows = new Map(); // winId -> state

        function bringToFront(winEl) {
            zCounter += 1;
            winEl.style.zIndex = String(zCounter);
            try {
                const iframe = winEl.querySelector('iframe');
                if (iframe) iframe.focus();
            } catch (_) {}
        }

        function closeAllDesktopWindows() {
            windowLayer.querySelectorAll('.desktop-window').forEach(function (winEl) {
                try {
                    const fr = winEl.querySelector('iframe');
                    if (fr && fr.contentWindow && fr.contentWindow.__rpcApp && fr.contentWindow.__rpcApp.stop) {
                        fr.contentWindow.__rpcApp.stop();
                    }
                } catch (_) {}
                winEl.remove();
            });
            windows.forEach(function (st) {
                if (st && st.taskBtn) st.taskBtn.remove();
                if (st && st.messageHandler) {
                    try {
                        window.removeEventListener('message', st.messageHandler);
                    } catch (_) {}
                }
            });
            windows.clear();
        }

        btnLogout.addEventListener('click', function (e) {
            e.stopPropagation();
            closeAllDesktopWindows();
            toggleStartMenu(true);
        });

        window.__rpcDesktop = {
            closeAll: closeAllDesktopWindows,
            getWindowCount: function () { return windows.size; },
        };

        function openAppWindow(app) {
            if (!app || (!app.exePath && !app.launchPath)) return;
            if (windows.size >= maxWindows) {
                console.warn('[desktop] 已达最大并发窗口数 (' + maxWindows + ')');
                return;
            }

            const winId = 'win_' + randomId(8);
            const idx = windows.size;
            const isMyData = app.kind === 'my_data' || (app.launchPath && app.launchPath.indexOf('my_data.html') >= 0);
            const isRpcVideoWindow = !isMyData;
            const viewportW = Math.max(800, window.innerWidth || 1280);
            const viewportH = Math.max(600, window.innerHeight || 720);
            const defaultW = isMyData ? Math.max(960, Math.floor(viewportW * 0.82)) : Math.max(640, Math.floor(viewportW * 0.7));
            const defaultH = isMyData ? Math.max(620, Math.floor((viewportH - 48) * 0.8)) : Math.max(360, Math.floor((viewportH - 48) * 0.7));
            const startLeft = (isMyData || isRpcVideoWindow)
                ? Math.max(12, Math.floor((viewportW - defaultW) / 2))
                : (40 + idx * 24);
            const startTop = (isMyData || isRpcVideoWindow)
                ? Math.max(12, Math.floor(((viewportH - 48) - defaultH) / 2))
                : (40 + idx * 24);

            const winEl = doc.createElement('div');
            winEl.className = 'desktop-window';
            winEl.dataset.winId = winId;
            winEl.style.position = 'absolute';
            winEl.style.left = String(startLeft) + 'px';
            winEl.style.top = String(startTop) + 'px';
            winEl.style.width = String(defaultW) + 'px';
            winEl.style.height = String(defaultH) + 'px';
            winEl.style.borderRadius = '12px';
            winEl.style.overflow = 'hidden';
            winEl.style.border = '1px solid rgba(255,255,255,0.10)';
            winEl.style.boxShadow = '0 16px 50px rgba(0,0,0,0.45)';
            winEl.style.background = '#000';
            winEl.style.zIndex = String(zCounter++);
            const titlebarHeight = isRpcVideoWindow ? 0 : 30;

            const titlebar = doc.createElement('div');
            titlebar.style.height = '30px';
            titlebar.style.display = 'flex';
            titlebar.style.alignItems = 'center';
            titlebar.style.justifyContent = 'space-between';
            titlebar.style.padding = '0 8px 0 10px';
            titlebar.style.background = 'linear-gradient(180deg, rgba(33,47,76,0.96), rgba(23,35,58,0.96))';
            titlebar.style.borderBottom = '1px solid rgba(255,255,255,0.08)';
            titlebar.style.cursor = 'move';

            const title = doc.createElement('div');
            title.textContent = app.name;
            title.style.color = '#e7eefc';
            title.style.fontWeight = '700';
            title.style.fontSize = '12px';
            title.style.letterSpacing = '0.2px';
            title.style.whiteSpace = 'nowrap';
            title.style.overflow = 'hidden';
            title.style.textOverflow = 'ellipsis';
            titlebar.appendChild(title);

            const btns = doc.createElement('div');
            btns.style.display = 'flex';
            btns.style.alignItems = 'center';
            btns.style.gap = '6px';
            titlebar.appendChild(btns);

            const minBtn = doc.createElement('button');
            minBtn.textContent = '—';
            minBtn.title = '最小化';
            minBtn.setAttribute('aria-label', '最小化');
            minBtn.style.width = '18px';
            minBtn.style.height = '18px';
            minBtn.style.borderRadius = '999px';
            minBtn.style.border = '1px solid rgba(255,255,255,0.26)';
            minBtn.style.background = '#f6c344';
            minBtn.style.boxShadow = 'inset 0 0 0 1px rgba(0,0,0,0.1)';
            minBtn.style.cursor = 'pointer';
            minBtn.style.padding = '0';
            minBtn.style.fontSize = '12px';
            minBtn.style.lineHeight = '16px';
            minBtn.style.fontWeight = '700';
            minBtn.style.color = 'rgba(60,45,0,0.75)';
            btns.appendChild(minBtn);

            const closeBtn = doc.createElement('button');
            closeBtn.textContent = '×';
            closeBtn.title = '关闭';
            closeBtn.setAttribute('aria-label', '关闭');
            closeBtn.style.width = '18px';
            closeBtn.style.height = '18px';
            closeBtn.style.borderRadius = '999px';
            closeBtn.style.border = '1px solid rgba(255,255,255,0.26)';
            closeBtn.style.background = '#ef6a64';
            closeBtn.style.boxShadow = 'inset 0 0 0 1px rgba(0,0,0,0.1)';
            closeBtn.style.cursor = 'pointer';
            closeBtn.style.padding = '0';
            closeBtn.style.fontSize = '12px';
            closeBtn.style.lineHeight = '16px';
            closeBtn.style.fontWeight = '700';
            closeBtn.style.color = 'rgba(80,0,0,0.75)';
            btns.appendChild(closeBtn);

            minBtn.addEventListener('mouseenter', function () { minBtn.style.filter = 'brightness(1.05)'; });
            minBtn.addEventListener('mouseleave', function () { minBtn.style.filter = 'none'; });
            closeBtn.addEventListener('mouseenter', function () { closeBtn.style.filter = 'brightness(1.05)'; });
            closeBtn.addEventListener('mouseleave', function () { closeBtn.style.filter = 'none'; });

            const content = doc.createElement('div');
            content.style.position = 'absolute';
            content.style.left = '0';
            content.style.right = '0';
            content.style.top = String(titlebarHeight) + 'px';
            content.style.bottom = '0';

            const iframe = doc.createElement('iframe');
            iframe.style.width = '100%';
            iframe.style.height = '100%';
            iframe.style.border = '0';
            iframe.style.display = 'block';
            iframe.tabIndex = 0;
            iframe.src = app.launchPath ? app.launchPath : makeIframeSrc(app.exePath);
            content.appendChild(iframe);

            const resizer = doc.createElement('div');
            resizer.style.position = 'absolute';
            resizer.style.right = '0';
            resizer.style.bottom = '0';
            resizer.style.width = '16px';
            resizer.style.height = '16px';
            resizer.style.cursor = 'nwse-resize';
            resizer.style.background = 'linear-gradient(135deg, transparent 0%, rgba(255,255,255,0.35) 100%)';
            resizer.style.opacity = '0.25';
            resizer.addEventListener('mouseenter', function () { resizer.style.opacity = '0.6'; });
            resizer.addEventListener('mouseleave', function () { resizer.style.opacity = '0.25'; });

            if (!isRpcVideoWindow) {
                winEl.appendChild(titlebar);
            }
            winEl.appendChild(content);
            if (!isRpcVideoWindow) {
                winEl.appendChild(resizer);
            }
            windowLayer.appendChild(winEl);

            // Drag
            let dragging = false;
            let dragStartX = 0, dragStartL = 0, dragStartY = 0, dragStartT = 0;
            let resizing = false;
            let resizeStartX = 0, resizeStartY = 0, resizeStartW = 0, resizeStartH = 0;

            function beginPointerInteraction(cursorStyle) {
                // 避免鼠标进入 iframe 后丢失 mouseup，导致“松开后仍在拖动/缩放”
                iframe.style.pointerEvents = 'none';
                doc.body.style.userSelect = 'none';
                doc.body.style.cursor = cursorStyle || 'default';
            }

            function endPointerInteraction() {
                dragging = false;
                resizing = false;
                iframe.style.pointerEvents = 'auto';
                doc.body.style.userSelect = '';
                doc.body.style.cursor = '';
            }

            titlebar.addEventListener('mousedown', function (e) {
                if (e.button !== 0) return;
                dragging = true;
                bringToFront(winEl);
                dragStartX = e.clientX;
                dragStartY = e.clientY;
                dragStartL = parseInt(winEl.style.left || '0', 10) || 0;
                dragStartT = parseInt(winEl.style.top || '0', 10) || 0;
                beginPointerInteraction('move');
                e.preventDefault();
            });
            doc.addEventListener('mousemove', function (e) {
                if (!dragging) return;
                const dx = e.clientX - dragStartX;
                const dy = e.clientY - dragStartY;
                winEl.style.left = String(dragStartL + dx) + 'px';
                winEl.style.top = String(dragStartT + dy) + 'px';
            });
            doc.addEventListener('mouseup', endPointerInteraction);
            window.addEventListener('mouseup', endPointerInteraction);
            window.addEventListener('blur', endPointerInteraction);

            // Resize (bottom-right)
            resizer.addEventListener('mousedown', function (e) {
                if (e.button !== 0) return;
                resizing = true;
                bringToFront(winEl);
                resizeStartX = e.clientX;
                resizeStartY = e.clientY;
                resizeStartW = parseInt(winEl.style.width || '640', 10) || 640;
                resizeStartH = parseInt(winEl.style.height || '400', 10) || 400;
                beginPointerInteraction('nwse-resize');
                e.preventDefault();
                e.stopPropagation();
            });
            doc.addEventListener('mousemove', function (e) {
                if (!resizing) return;
                const dx = e.clientX - resizeStartX;
                const dy = e.clientY - resizeStartY;
                const nw = Math.max(320, resizeStartW + dx);
                const nh = Math.max(220, resizeStartH + dy);
                winEl.style.width = String(nw) + 'px';
                winEl.style.height = String(nh) + 'px';
            });
            doc.addEventListener('mouseup', endPointerInteraction);

            // Focus
            winEl.addEventListener('mousedown', function () { bringToFront(winEl); });

            const taskIconText = app.icon || '';
            const taskNameText = app.name;

            function minimize() {
                const st = windows.get(winId);
                if (!st) return;
                st.minimized = true;
                st.lastBounds = st.lastBounds || {
                    left: winEl.style.left,
                    top: winEl.style.top,
                    width: winEl.style.width,
                    height: winEl.style.height,
                };
                // Keep iframe alive: just hide window visually.
                winEl.style.opacity = '0';
                winEl.style.pointerEvents = 'none';
            }

            function restore() {
                const st = windows.get(winId);
                if (!st) return;
                st.minimized = false;
                const b = st.lastBounds;
                if (b) {
                    winEl.style.left = b.left;
                    winEl.style.top = b.top;
                    winEl.style.width = b.width;
                    winEl.style.height = b.height;
                }
                winEl.style.opacity = '1';
                winEl.style.pointerEvents = 'auto';
                bringToFront(winEl);
            }

            function fitWindowToVideoResolution(videoW, videoH) {
                if (!isRpcVideoWindow) return;
                const vw = Number(videoW) || 0;
                const vh = Number(videoH) || 0;
                if (!vw || !vh) return;
                const stageH = Math.max(360, (window.innerHeight || 720) - 48);
                const maxW = Math.max(640, Math.floor((window.innerWidth || 1280) * 0.9));
                const maxH = Math.max(360, Math.floor(stageH * 0.9));
                let targetW = vw;
                let targetH = vh;
                const scale = Math.min(maxW / targetW, maxH / targetH, 1);
                targetW = Math.round(targetW * scale);
                targetH = Math.round(targetH * scale);
                winEl.style.width = String(Math.max(320, targetW)) + 'px';
                winEl.style.height = String(Math.max(180, targetH)) + 'px';
                const left = Math.max(12, Math.floor(((window.innerWidth || 1280) - targetW) / 2));
                const top = Math.max(12, Math.floor((stageH - targetH) / 2));
                winEl.style.left = String(left) + 'px';
                winEl.style.top = String(top) + 'px';
            }

            function onChildFrameMessage(event) {
                if (!event || !event.source || event.source !== iframe.contentWindow) return;
                const data = event.data;
                if (!data || !data.type) return;
                if (data.type === 'rpc_video_resolution') {
                    fitWindowToVideoResolution(data.width, data.height);
                    return;
                }
                if (data.type === 'rpc_request_close') {
                    closeWindow();
                }
            }
            window.addEventListener('message', onChildFrameMessage);

            const taskBtn = doc.createElement('div');
            taskBtn.textContent = taskIconText ? (taskIconText + ' ' + taskNameText) : taskNameText;
            taskBtn.style.flex = '0 0 auto';
            taskBtn.style.height = '34px';
            taskBtn.style.padding = '0 10px';
            taskBtn.style.display = 'flex';
            taskBtn.style.alignItems = 'center';
            taskBtn.style.borderRadius = '10px';
            taskBtn.style.border = '1px solid rgba(255,255,255,0.08)';
            taskBtn.style.background = '#182033';
            taskBtn.style.color = '#e7eefc';
            taskBtn.style.fontSize = '12px';
            taskBtn.style.cursor = 'pointer';
            taskBtn.style.whiteSpace = 'nowrap';
            taskBtn.style.userSelect = 'none';
            taskBtn.addEventListener('click', function () {
                const st = windows.get(winId);
                if (!st) return;
                if (st.minimized) restore();
                else minimize();
            });
            taskBtn.addEventListener('contextmenu', function (e) {
                e.preventDefault();
                closeWindow();
            });
            appsStrip.appendChild(taskBtn);

            function closeWindow() {
                const st = windows.get(winId);
                try {
                    if (iframe && iframe.contentWindow && iframe.contentWindow.__rpcApp && iframe.contentWindow.__rpcApp.stop) {
                        iframe.contentWindow.__rpcApp.stop();
                    }
                } catch (_) {}
                try {
                    window.removeEventListener('message', onChildFrameMessage);
                } catch (_) {}
                if (st && st.taskBtn) st.taskBtn.remove();
                windows.delete(winId);
                winEl.remove();
            }

            minBtn.addEventListener('click', function (e) { e.stopPropagation(); minimize(); });
            closeBtn.addEventListener('click', function (e) {
                e.stopPropagation();
                closeWindow();
            });

            windows.set(winId, {
                winId: winId,
                app: app,
                minEl: minBtn,
                taskBtn: taskBtn,
                minimized: false,
                lastBounds: null,
                messageHandler: onChildFrameMessage,
            });
            bringToFront(winEl);
        }

        // Expose a tiny helper for debug.
        window.__rpcDesktopOpen = openAppWindow;
    };
})();

