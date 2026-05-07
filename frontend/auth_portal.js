/**
 * 科技感登录门户：成功后写入 window.__rpcAuthCatalog，再启动桌面。
 */
(function (global) {
    'use strict';

    var STORAGE_TOKEN = 'rpc_auth_token_v1';
    var STORAGE_USER = 'rpc_auth_username_v1';
    var STORAGE_ROLE = 'rpc_auth_role_v1';
    function readStored() {
        try {
            return {
                token: global.sessionStorage.getItem(STORAGE_TOKEN) || '',
                username: global.sessionStorage.getItem(STORAGE_USER) || '',
                role: global.sessionStorage.getItem(STORAGE_ROLE) || '',
            };
        } catch (_) {
            return { token: '', username: '', role: '' };
        }
    }

    function persistSession(username, role, token) {
        try {
            global.sessionStorage.setItem(STORAGE_TOKEN, token || '');
            global.sessionStorage.setItem(STORAGE_USER, username || '');
            global.sessionStorage.setItem(STORAGE_ROLE, role || '');
        } catch (_) {}
    }

    function clearSession() {
        try {
            global.sessionStorage.removeItem(STORAGE_TOKEN);
            global.sessionStorage.removeItem(STORAGE_USER);
            global.sessionStorage.removeItem(STORAGE_ROLE);
        } catch (_) {}
        global.__rpcAuthCatalog = null;
        global.__rpcAuthUsername = '';
        global.__rpcAuthRole = '';
    }

    function finishAndEnter(doc, onReady, catalog, username, role) {
        global.__rpcAuthCatalog = catalog || { apps: [], iconsByAppId: {} };
        global.__rpcAuthUsername = username || '';
        global.__rpcAuthRole = role || '';
        var root = doc.getElementById('rpc-auth-root');
        if (root) root.classList.add('rpc-auth-root--hidden');
        onReady();
    }

    function setStatus(el, text, ok) {
        if (!el) return;
        el.textContent = text || '';
        el.classList.toggle('rpc-auth-status--ok', !!ok);
    }

    function mountLoginForm(doc, onReady) {
        var root = doc.createElement('div');
        root.id = 'rpc-auth-root';
        root.innerHTML =
            '<div class="rpc-auth-panel">' +
            '<div class="rpc-auth-panel-inner">' +
            '<div class="rpc-auth-brand">' +
            '<div class="rpc-auth-brand-mark">工业软件远程工作站</div>' +
            '<div class="rpc-auth-brand-title">统一接入 · 远程会话</div>' +
            '<div class="rpc-auth-brand-sub">Industrial Software Remote Workstation · WebRTC Control Plane</div>' +
            '</div>' +
            '<div class="rpc-auth-field"><label>用户标识</label>' +
            '<input type="text" id="rpc-auth-user" autocomplete="username" placeholder="Operator ID" /></div>' +
            '<div class="rpc-auth-field"><label>访问密钥</label>' +
            '<input type="password" id="rpc-auth-pass" autocomplete="current-password" placeholder="••••••••" /></div>' +
            '<div class="rpc-auth-actions">' +
            '<button type="button" class="rpc-auth-btn-primary" id="rpc-auth-submit">建立加密链路</button>' +
            '</div>' +
            '<div class="rpc-auth-status" id="rpc-auth-status"></div>' +
            '</div></div>';
        doc.body.appendChild(root);

        var inpUser = doc.getElementById('rpc-auth-user');
        var inpPass = doc.getElementById('rpc-auth-pass');
        var btn = doc.getElementById('rpc-auth-submit');
        var statusEl = doc.getElementById('rpc-auth-status');

        btn.addEventListener('click', function () {
            var u = inpUser && inpUser.value ? String(inpUser.value).trim() : '';
            var p = inpPass && inpPass.value ? String(inpPass.value) : '';
            if (!u || !p) {
                setStatus(statusEl, '请输入用户名与密码', false);
                return;
            }
            setStatus(statusEl, '同步授权目录…', false);
            btn.disabled = true;
            var api = global.__rpcAuthSignaling;
            if (!api || typeof api.loginAndFetchCatalog !== 'function') {
                setStatus(statusEl, 'auth_signaling 未加载', false);
                btn.disabled = false;
                return;
            }
            var ensure = global.__rpcEnsureSignalingJsonLoaded
                ? global.__rpcEnsureSignalingJsonLoaded()
                : Promise.resolve(false);
            ensure
                .then(function () {
                    return api.loginAndFetchCatalog(u, p);
                })
                .then(function (pack) {
                    persistSession(u, pack.role || '', pack.token || '');
                    var catalog = {
                        apps: pack.apps || [],
                        iconsByAppId: pack.iconsByAppId || {},
                        token: pack.token,
                    };
                    setStatus(statusEl, '链路就绪', true);
                    global.setTimeout(function () {
                        finishAndEnter(doc, onReady, catalog, u, pack.role || '');
                    }, 220);
                })
                .catch(function (err) {
                    console.warn('[rpc-auth]', err);
                    setStatus(statusEl, (err && err.message) || '登录失败', false);
                    btn.disabled = false;
                });
        });

        if (inpPass) {
            inpPass.addEventListener('keydown', function (e) {
                if (e.key === 'Enter') btn.click();
            });
        }
    }

    /**
     * @param {Document} doc
     * @param {() => void} onReady
     */
    function mountBeforeDesktop(doc, onReady) {
        if (!doc || typeof onReady !== 'function') return;

        var st = readStored();

        var api = global.__rpcAuthSignaling;
        if (st.token && st.username && api && typeof api.fetchCatalogWithToken === 'function') {
            var ensure = global.__rpcEnsureSignalingJsonLoaded
                ? global.__rpcEnsureSignalingJsonLoaded()
                : Promise.resolve(false);
            ensure
                .then(function () {
                    return api.fetchCatalogWithToken(st.token);
                })
                .then(function (cat) {
                    finishAndEnter(
                        doc,
                        onReady,
                        {
                            apps: cat.apps || [],
                            iconsByAppId: cat.iconsByAppId || {},
                            token: st.token,
                        },
                        st.username,
                        st.role || ''
                    );
                })
                .catch(function () {
                    clearSession();
                    mountLoginForm(doc, onReady);
                });
            return;
        }

        mountLoginForm(doc, onReady);
    }

    global.__rpcMountAuthPortal = mountBeforeDesktop;
    global.__rpcAuthClearSession = clearSession;
})(typeof window !== 'undefined' ? window : this);
