/**
 * 工业软件远程工作站 — 管理员管控台（统计 / 节点 / 应用 / 人员）
 * 依赖：auth_signaling.js、__rpcMd5Hex、__rpcAuthCatalog.token、__rpcAuthRole
 */
(function (global) {
    'use strict';

    var sessionHist = [];
    var MAX_HIST = 48;
    var autoTimer = null;

    function appIdToName(id) {
        var cat = global.__rpcAuthCatalog;
        if (!cat || !Array.isArray(cat.apps)) return '#' + id;
        var nid = Number(id);
        for (var i = 0; i < cat.apps.length; i++) {
            var a = cat.apps[i];
            if (a && Number(a.app_id) === nid) {
                return String(a.display_name || a.node_name || '#' + id);
            }
        }
        return '应用 #' + id;
    }

    function fmtTotalTime(v) {
        var n = Number(v) || 0;
        if (n <= 0) return '—';
        if (n >= 1000000) return (n / 3600000).toFixed(2) + ' 小时';
        if (n >= 100000) return (n / 60000).toFixed(1) + ' 分钟';
        if (n >= 1000) return (n / 60000).toFixed(1) + ' 分钟';
        return n + ' 秒';
    }

    function pwdTransport(plain) {
        if (typeof global.__rpcMd5Hex === 'function') {
            return global.__rpcMd5Hex(String(plain || ''));
        }
        return String(plain || '');
    }

    function drawSparkline(canvas, values) {
        if (!canvas || !canvas.getContext) return;
        var dpr = Math.min(global.devicePixelRatio || 1, 2);
        var cw = canvas.clientWidth || 800;
        var ch = canvas.clientHeight || 120;
        canvas.width = Math.floor(cw * dpr);
        canvas.height = Math.floor(ch * dpr);
        var ctx = canvas.getContext('2d');
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, cw, ch);
        ctx.fillStyle = 'rgba(0,30,50,0.45)';
        ctx.fillRect(0, 0, cw, ch);
        if (!values || values.length < 2) {
            return;
        }
        var min = Math.min.apply(null, values);
        var max = Math.max.apply(null, values);
        var pad = 10;
        if (max === min) {
            min -= 1;
            max += 1;
        }
        ctx.strokeStyle = 'rgba(0,220,255,0.9)';
        ctx.lineWidth = 2;
        ctx.beginPath();
        var xs = [];
        var ys = [];
        for (var i = 0; i < values.length; i++) {
            var xi = pad + (i / (values.length - 1)) * (cw - pad * 2);
            var yi = pad + (1 - (values[i] - min) / (max - min)) * (ch - pad * 2);
            xs.push(xi);
            ys.push(yi);
            if (i === 0) ctx.moveTo(xi, yi);
            else ctx.lineTo(xi, yi);
        }
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(xs[0], ch - pad);
        for (var k = 0; k < xs.length; k++) {
            ctx.lineTo(xs[k], ys[k]);
        }
        ctx.lineTo(xs[xs.length - 1], ch - pad);
        ctx.closePath();
        ctx.fillStyle = 'rgba(0,200,255,0.12)';
        ctx.fill();
    }

    function renderBars(host, ranking) {
        host.innerHTML = '';
        if (!ranking || !ranking.length) {
            host.innerHTML = '<div class="rpc-admin-chart-note">暂无数据</div>';
            return;
        }
        var maxT = 0;
        for (var i = 0; i < ranking.length; i++) {
            maxT = Math.max(maxT, Number(ranking[i].total_time) || 0);
        }
        if (maxT <= 0) maxT = 1;
        var BAR_MAX_PX = 168;
        for (var j = 0; j < ranking.length; j++) {
            var item = ranking[j];
            var tid = item.app_id != null ? item.app_id : 0;
            var tv = Number(item.total_time) || 0;
            var pct = tv / maxT;
            var px = Math.max(8, Math.round(pct * BAR_MAX_PX));
            var col = document.createElement('div');
            col.className = 'rpc-admin-bar-col';
            var bar = document.createElement('div');
            bar.className = 'rpc-admin-bar';
            bar.style.height = px + 'px';
            bar.style.flexShrink = '0';
            bar.title = fmtTotalTime(tv);
            var lb = document.createElement('div');
            lb.className = 'rpc-admin-bar-label';
            lb.textContent = appIdToName(tid);
            var tvEl = document.createElement('div');
            tvEl.className = 'rpc-admin-bar-label';
            tvEl.style.fontSize = '9px';
            tvEl.style.opacity = '0.75';
            tvEl.textContent = fmtTotalTime(tv);
            col.appendChild(bar);
            col.appendChild(lb);
            col.appendChild(tvEl);
            host.appendChild(col);
        }
    }

    function getToken() {
        var c = global.__rpcAuthCatalog;
        return c && c.token ? String(c.token) : '';
    }

    function refreshStats(root) {
        var api = global.__rpcAuthSignaling;
        var tok = getToken();
        var statusEl = root.querySelector('#rpc-admin-fetch-status');
        var kpiAct = root.querySelector('#rpc-admin-kpi-active');
        var kpiNodes = root.querySelector('#rpc-admin-kpi-nodes');
        var nodesHost = root.querySelector('#rpc-admin-nodes');
        var barsHost = root.querySelector('#rpc-admin-bars');
        var canvas = root.querySelector('#rpc-admin-spark');
        if (!api || typeof api.fetchAdminStats !== 'function') {
            if (statusEl) statusEl.textContent = 'auth_signaling 未就绪';
            return;
        }
        if (!tok) {
            if (statusEl) statusEl.textContent = '无登录令牌';
            return;
        }
        if (statusEl) {
            statusEl.textContent = '加载中…';
            statusEl.classList.remove('rpc-admin-status--ok');
        }
        api.fetchAdminStats(tok)
            .then(function (stats) {
                if (kpiAct) kpiAct.textContent = String(stats.active_sessions);
                if (kpiNodes) kpiNodes.textContent = String(stats.nodes_online.length);
                if (nodesHost) {
                    nodesHost.innerHTML = '';
                    stats.nodes_online.forEach(function (n) {
                        var chip = document.createElement('span');
                        chip.className = 'rpc-admin-chip';
                        chip.textContent = n;
                        nodesHost.appendChild(chip);
                    });
                    if (!stats.nodes_online.length) {
                        nodesHost.innerHTML = '<span class="rpc-admin-chart-note">无在线节点</span>';
                    }
                }
                renderBars(barsHost, stats.usage_ranking);
                sessionHist.push(stats.active_sessions);
                if (sessionHist.length > MAX_HIST) sessionHist.shift();
                drawSparkline(canvas, sessionHist);
                if (statusEl) {
                    statusEl.textContent = '已更新 ' + new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
                    statusEl.classList.add('rpc-admin-status--ok');
                }
            })
            .catch(function (e) {
                if (statusEl) {
                    statusEl.textContent = (e && e.message) || '拉取统计失败';
                    statusEl.classList.remove('rpc-admin-status--ok');
                }
            });
    }

    function yn(v) {
        var n = Number(v);
        if (n === 1) return '是';
        if (n === 0) return '否';
        return String(v != null ? v : '—');
    }

    function makeMiniBtn(text, className) {
        var b = document.createElement('button');
        b.type = 'button';
        b.className = className || 'rpc-admin-btn rpc-admin-btn-mini';
        b.textContent = text;
        return b;
    }

    function readIconFileAsBase64(file, onOk, onErr) {
        if (!file) {
            if (onErr) onErr('未选择文件');
            return;
        }
        var okMime =
            /^(image\/(png|svg\+xml|x-icon|vnd\.microsoft\.icon))$/i.test(String(file.type || '')) ||
            /^image\/.*icon/i.test(String(file.type || ''));
        var okExt = /\.(ico|png|svg)$/i.test(String(file.name || ''));
        if (!okMime && !okExt) {
            if (onErr) onErr('请上传图标文件：.ico、.png 或 .svg');
            return;
        }
        if (file.size > 2 * 1024 * 1024) {
            if (onErr) onErr('图标文件请小于 2MB');
            return;
        }
        var r = new FileReader();
        r.onload = function () {
            var s = r.result;
            if (typeof s !== 'string' || s.indexOf(',') < 0) {
                if (onErr) onErr('读取失败');
                return;
            }
            onOk(s.split(',')[1] || '');
        };
        r.onerror = function () {
            if (onErr) onErr('读取失败');
        };
        r.readAsDataURL(file);
    }

    function fillAppNodeSelect(root, dir) {
        var sel = root.querySelector('#rpc-admin-in-node');
        if (!sel || String(sel.tagName).toLowerCase() !== 'select') return;
        var cur = sel.value;
        sel.innerHTML = '';
        var z = document.createElement('option');
        z.value = '';
        z.textContent = '— 请选择采集节点 —';
        sel.appendChild(z);
        (dir.nodes || []).forEach(function (n) {
            var name = String(n.node_name || '').trim();
            if (!name) return;
            var o = document.createElement('option');
            o.value = name;
            o.textContent = name;
            sel.appendChild(o);
        });
        try {
            if (cur) sel.value = cur;
        } catch (_) {}
    }

    function formatAdminMutateError(msg) {
        var m = String(msg || '');
        var uidM = /^username_exists(?: uid=(\d+))?$/.exec(m);
        if (uidM) {
            return uidM[1] ? '用户名已存在（#' + uidM[1] + '）' : '用户名已存在';
        }
        var map = {
            forbidden: '无权限执行此操作',
            invalid: '请检查填写项是否完整、有效',
            invalid_role: '角色无效，仅支持管理员或普通用户',
            not_found: '记录不存在或已被删除',
            cannot_delete_self: '不能删除当前登录的管理员自身',
            db_error: '数据库错误，请稍后重试',
            constraint: '与数据库约束冲突，请检查输入或联系维护人员',
        };
        return map[m] != null ? map[m] : m;
    }

    function setPeopleUserEditingId(root, rawId) {
        var n =
            rawId == null || rawId === ''
                ? NaN
                : typeof rawId === 'number'
                  ? rawId
                  : Number(String(rawId).trim());
        root._rpcAdminUserEditingId = !isNaN(n) && n > 0 ? n : null;
        var hid = root.querySelector('#rpc-admin-user-edit-id');
        if (hid) hid.value = root._rpcAdminUserEditingId != null ? String(root._rpcAdminUserEditingId) : '';
    }

    function syncUserFormModeHint(root) {
        var el = root.querySelector('#rpc-admin-user-mode-hint');
        if (!el) return;
        var id = root._rpcAdminUserEditingId;
        var editing = id != null && typeof id === 'number' && id > 0;
        el.textContent = editing ? '编辑用户 #' + id : '新建用户';
    }

    function resetPeopleUserForm(root) {
        setPeopleUserEditingId(root, null);
        var nameEl = root.querySelector('#rpc-admin-user-name');
        if (nameEl) nameEl.value = '';
        var passEl = root.querySelector('#rpc-admin-user-pass');
        if (passEl) passEl.value = '';
        var roleEl = root.querySelector('#rpc-admin-user-role');
        if (roleEl) {
            roleEl.innerHTML =
                '<option value="user">普通用户</option>' +
                '<option value="admin">管理员</option>';
            roleEl.value = 'user';
        }
        var st = root.querySelector('#rpc-admin-user-form-status');
        if (st) {
            st.textContent = '';
            st.classList.remove('rpc-admin-status--ok');
        }
        syncUserFormModeHint(root);
    }

    function setUserRoleSelectValue(root, roleFromDb) {
        var roleEl = root.querySelector('#rpc-admin-user-role');
        if (!roleEl) return;
        roleEl.innerHTML =
            '<option value="user">普通用户</option>' +
            '<option value="admin">管理员</option>';
        var r = String(roleFromDb != null ? roleFromDb : 'user').trim();
        if (r && r !== 'user' && r !== 'admin') {
            var o = document.createElement('option');
            o.value = r;
            o.textContent = r + '（自定义）';
            roleEl.appendChild(o);
        }
        roleEl.value = r || 'user';
    }

    function fillPermSelects(root, dir) {
        var su = root.querySelector('#rpc-admin-perm-user');
        var sa = root.querySelector('#rpc-admin-perm-app');
        if (!su || !sa) return;
        var uv = su.value;
        var av = sa.value;
        su.innerHTML = '';
        var z = document.createElement('option');
        z.value = '';
        z.textContent = '— 选择用户 —';
        su.appendChild(z);
        (dir.users || []).forEach(function (u) {
            var o = document.createElement('option');
            o.value = String(u.user_id);
            o.textContent = String(u.username || '') + ' (' + u.user_id + ')';
            su.appendChild(o);
        });
        sa.innerHTML = '';
        var z2 = document.createElement('option');
        z2.value = '';
        z2.textContent = '— 选择应用 —';
        sa.appendChild(z2);
        (dir.apps || []).forEach(function (a) {
            var o = document.createElement('option');
            o.value = String(a.app_id);
            o.textContent = String(a.display_name || 'app') + ' #' + a.app_id;
            sa.appendChild(o);
        });
        try {
            if (uv) su.value = uv;
            if (av) sa.value = av;
        } catch (_) {}
    }

    function syncCatalogAndDesktop(tok, api) {
        var fetchCat = api.fetchCatalogWithToken;
        if (typeof fetchCat !== 'function' || !tok) return Promise.resolve();
        return fetchCat(tok).then(function (cat) {
            global.__rpcAuthCatalog = global.__rpcAuthCatalog || {};
            global.__rpcAuthCatalog.apps = cat.apps || [];
            global.__rpcAuthCatalog.iconsByAppId = cat.iconsByAppId || {};
            global.__rpcAuthCatalog.token = tok;
            if (typeof global.__rpcDesktopRefreshApps === 'function') {
                global.__rpcDesktopRefreshApps();
            }
        });
    }

    function refreshDirectory(root) {
        var api = global.__rpcAuthSignaling;
        var tok = getToken();
        var statusEl = root.querySelector('#rpc-admin-dir-status');
        var tbNodes = root.querySelector('#rpc-admin-tbody-nodes');
        var tbApps = root.querySelector('#rpc-admin-tbody-apps');
        var tbUsers = root.querySelector('#rpc-admin-tbody-users');
        var tbPerm = root.querySelector('#rpc-admin-tbody-perms');
        if (!api || typeof api.fetchAdminDirectory !== 'function') {
            if (statusEl) statusEl.textContent = 'fetchAdminDirectory 不可用';
            return;
        }
        if (!tok) {
            if (statusEl) statusEl.textContent = '无登录令牌';
            return;
        }
        if (statusEl) {
            statusEl.textContent = '加载中…';
            statusEl.classList.remove('rpc-admin-status--ok');
        }
        api.fetchAdminDirectory(tok)
            .then(function (dir) {
                root._rpcAdminLastDir = dir;
                fillPermSelects(root, dir);
                fillAppNodeSelect(root, dir);

                if (tbNodes) {
                    tbNodes.innerHTML = '';
                    (dir.nodes || []).forEach(function (n) {
                        var tr = document.createElement('tr');
                        [
                            String(n.node_id != null ? n.node_id : ''),
                            String(n.node_name || ''),
                            yn(n.is_online),
                            String(n.last_seen || '—'),
                        ].forEach(function (cell) {
                            var td = document.createElement('td');
                            td.textContent = cell;
                            tr.appendChild(td);
                        });
                        var tdAct = document.createElement('td');
                        tdAct.className = 'rpc-admin-table-actions';
                        var bEd = makeMiniBtn('编辑');
                        var bDel = makeMiniBtn('删除', 'rpc-admin-btn rpc-admin-btn-mini rpc-admin-btn-mini--danger');
                        bEd.addEventListener('click', function () {
                            var idEl = root.querySelector('#rpc-admin-node-edit-id');
                            var nameEl = root.querySelector('#rpc-admin-node-name');
                            var keyEl = root.querySelector('#rpc-admin-node-key');
                            if (idEl) idEl.value = String(n.node_id || '');
                            if (nameEl) nameEl.value = String(n.node_name || '');
                            if (keyEl) keyEl.value = '';
                        });
                        bDel.addEventListener('click', function () {
                            if (!global.confirm('确定删除节点 #' + n.node_id + ' ?')) return;
                            if (!api.adminMutate) return;
                            api.adminMutate(tok, {
                                entity: 'node',
                                action: 'delete',
                                payload: { node_id: Number(n.node_id) },
                            })
                                .then(function () {
                                    return refreshDirectory(root);
                                })
                                .catch(function (e) {
                                    global.alert((e && e.message) || '删除失败');
                                });
                        });
                        tdAct.appendChild(bEd);
                        tdAct.appendChild(bDel);
                        tr.appendChild(tdAct);
                        tbNodes.appendChild(tr);
                    });
                }
                if (tbApps) {
                    tbApps.innerHTML = '';
                    (dir.apps || []).forEach(function (a) {
                        var tr = document.createElement('tr');
                        [
                            String(a.app_id != null ? a.app_id : ''),
                            String(a.node_name || ''),
                            String(a.display_name || ''),
                            String(a.exe_path || ''),
                            yn(a.is_public),
                        ].forEach(function (cell) {
                            var td = document.createElement('td');
                            td.textContent = cell;
                            tr.appendChild(td);
                        });
                        var tdAct = document.createElement('td');
                        tdAct.className = 'rpc-admin-table-actions';
                        var bEd = makeMiniBtn('编辑');
                        var bDel = makeMiniBtn('删除', 'rpc-admin-btn rpc-admin-btn-mini rpc-admin-btn-mini--danger');
                        bEd.addEventListener('click', function () {
                            var hid = root.querySelector('#rpc-admin-app-edit-id');
                            var node = root.querySelector('#rpc-admin-in-node');
                            var title = root.querySelector('#rpc-admin-in-title');
                            var exe = root.querySelector('#rpc-admin-in-exe');
                            var icon = root.querySelector('#rpc-admin-in-icon');
                            var iconFile = root.querySelector('#rpc-admin-in-icon-file');
                            var iconName = root.querySelector('#rpc-admin-in-icon-name');
                            var pub = root.querySelector('#rpc-admin-in-public');
                            var sub = root.querySelector('#rpc-admin-submit-app');
                            if (hid) hid.value = String(a.app_id || '');
                            if (node) node.value = String(a.node_name || '');
                            if (title) title.value = String(a.display_name || '');
                            if (exe) exe.value = String(a.exe_path || '');
                            if (icon) icon.value = '';
                            if (iconFile) iconFile.value = '';
                            if (iconName) iconName.textContent = '';
                            if (pub) pub.checked = Number(a.is_public) === 1;
                            if (sub) sub.textContent = '保存';
                        });
                        bDel.addEventListener('click', function () {
                            if (!global.confirm('确定删除应用 #' + a.app_id + ' ?')) return;
                            api.adminMutate(tok, {
                                entity: 'application',
                                action: 'delete',
                                payload: { app_id: Number(a.app_id) },
                            })
                                .then(function () {
                                    return syncCatalogAndDesktop(tok, api).then(function () {
                                        refreshDirectory(root);
                                        refreshStats(root);
                                    });
                                })
                                .catch(function (e) {
                                    global.alert((e && e.message) || '删除失败');
                                });
                        });
                        tdAct.appendChild(bEd);
                        tdAct.appendChild(bDel);
                        tr.appendChild(tdAct);
                        tbApps.appendChild(tr);
                    });
                }
                if (tbUsers) {
                    tbUsers.innerHTML = '';
                    (dir.users || []).forEach(function (u) {
                        var tr = document.createElement('tr');
                        [
                            String(u.user_id != null ? u.user_id : ''),
                            String(u.username || ''),
                            String(u.role || ''),
                        ].forEach(function (cell) {
                            var td = document.createElement('td');
                            td.textContent = cell;
                            tr.appendChild(td);
                        });
                        var tdAct = document.createElement('td');
                        tdAct.className = 'rpc-admin-table-actions';
                        var bEd = makeMiniBtn('编辑');
                        var bDel = makeMiniBtn('删除', 'rpc-admin-btn rpc-admin-btn-mini rpc-admin-btn-mini--danger');
                        bEd.addEventListener('click', function () {
                            var nameEl = root.querySelector('#rpc-admin-user-name');
                            var passEl = root.querySelector('#rpc-admin-user-pass');
                            setPeopleUserEditingId(root, u.user_id);
                            if (nameEl) nameEl.value = String(u.username || '');
                            if (passEl) passEl.value = '';
                            setUserRoleSelectValue(root, u.role);
                            syncUserFormModeHint(root);
                        });
                        bDel.addEventListener('click', function () {
                            if (!global.confirm('确定删除用户 #' + u.user_id + ' ?')) return;
                            api.adminMutate(tok, {
                                entity: 'user',
                                action: 'delete',
                                payload: { user_id: Number(u.user_id) },
                            })
                                .then(function () {
                                    refreshDirectory(root);
                                })
                                .catch(function (e) {
                                    global.alert((e && e.message) || '删除失败');
                                });
                        });
                        tdAct.appendChild(bEd);
                        tdAct.appendChild(bDel);
                        tr.appendChild(tdAct);
                        tbUsers.appendChild(tr);
                    });
                }
                if (tbPerm) {
                    tbPerm.innerHTML = '';
                    (dir.permissions || []).forEach(function (p) {
                        var tr = document.createElement('tr');
                        [
                            String(p.perm_id != null ? p.perm_id : ''),
                            String(p.username || ''),
                            String(p.app_id != null ? p.app_id : ''),
                            String(p.app_display_name || ''),
                            String(p.node_name || ''),
                        ].forEach(function (cell) {
                            var td = document.createElement('td');
                            td.textContent = cell;
                            tr.appendChild(td);
                        });
                        var tdAct = document.createElement('td');
                        tdAct.className = 'rpc-admin-table-actions';
                        var bEd = makeMiniBtn('编辑');
                        var bDel = makeMiniBtn('删除', 'rpc-admin-btn rpc-admin-btn-mini rpc-admin-btn-mini--danger');
                        bEd.addEventListener('click', function () {
                            var pid = root.querySelector('#rpc-admin-perm-edit-id');
                            var su = root.querySelector('#rpc-admin-perm-user');
                            var sa = root.querySelector('#rpc-admin-perm-app');
                            if (pid) pid.value = String(p.perm_id || '');
                            if (su) su.value = String(p.user_id || '');
                            if (sa) sa.value = String(p.app_id || '');
                        });
                        bDel.addEventListener('click', function () {
                            if (!global.confirm('确定删除授权 #' + p.perm_id + ' ?')) return;
                            api.adminMutate(tok, {
                                entity: 'permission',
                                action: 'delete',
                                payload: { perm_id: Number(p.perm_id) },
                            })
                                .then(function () {
                                    refreshDirectory(root);
                                })
                                .catch(function (e) {
                                    global.alert((e && e.message) || '删除失败');
                                });
                        });
                        tdAct.appendChild(bEd);
                        tdAct.appendChild(bDel);
                        tr.appendChild(tdAct);
                        tbPerm.appendChild(tr);
                    });
                }
                if (statusEl) {
                    var nn = (dir.nodes || []).length;
                    var na = (dir.apps || []).length;
                    var nu = (dir.users || []).length;
                    var np = (dir.permissions || []).length;
                    statusEl.textContent =
                        '已同步 ' +
                        new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }) +
                        ' · 节点' +
                        nn +
                        ' 应用' +
                        na +
                        ' 用户' +
                        nu +
                        ' 授权' +
                        np;
                    statusEl.classList.add('rpc-admin-status--ok');
                }
            })
            .catch(function (e) {
                if (statusEl) {
                    statusEl.textContent = (e && e.message) || '拉取目录失败';
                    statusEl.classList.remove('rpc-admin-status--ok');
                }
            });
    }

    function stopAuto() {
        if (autoTimer) {
            clearInterval(autoTimer);
            autoTimer = null;
        }
    }

    function getActiveTabKey(root) {
        var a = root.querySelector('.rpc-admin-nav-item.rpc-admin-nav-item--active');
        return a ? a.getAttribute('data-admin-tab') : 'stats';
    }

    function setTab(root, key) {
        root.querySelectorAll('.rpc-admin-nav-item').forEach(function (b) {
            var k = b.getAttribute('data-admin-tab');
            if (k === key) b.classList.add('rpc-admin-nav-item--active');
            else b.classList.remove('rpc-admin-nav-item--active');
        });
        var pageByTab = {
            stats: 'rpc-admin-page-stats',
            nodes: 'rpc-admin-page-nodes',
            apps: 'rpc-admin-page-apps',
            people: 'rpc-admin-page-people',
        };
        Object.keys(pageByTab).forEach(function (k) {
            var sec = root.querySelector('#' + pageByTab[k]);
            if (!sec) return;
            if (k === key) sec.removeAttribute('hidden');
            else sec.setAttribute('hidden', '');
        });
        var dirMeta = root.querySelector('#rpc-admin-dir-meta');
        if (dirMeta) {
            if (key === 'nodes' || key === 'apps' || key === 'people') dirMeta.removeAttribute('hidden');
            else dirMeta.setAttribute('hidden', '');
        }
        if (key === 'stats') refreshStats(root);
        if (key === 'nodes' || key === 'apps' || key === 'people') refreshDirectory(root);
    }

    function wireCrudForms(root) {
        var api = global.__rpcAuthSignaling;
        var tok = getToken();

        root.querySelector('#rpc-admin-node-save').addEventListener('click', function () {
            var st = root.querySelector('#rpc-admin-node-form-status');
            if (!api || !api.adminMutate) {
                if (st) st.textContent = 'adminMutate 不可用';
                return;
            }
            var editId = root.querySelector('#rpc-admin-node-edit-id');
            var nameEl = root.querySelector('#rpc-admin-node-name');
            var keyEl = root.querySelector('#rpc-admin-node-key');
            var node_name = nameEl && nameEl.value ? nameEl.value.trim() : '';
            var auth_key = keyEl && keyEl.value ? keyEl.value : '';
            var isEdit = editId && editId.value && String(editId.value).trim().length > 0;
            if (!node_name) {
                if (st) st.textContent = '请填写节点名';
                return;
            }
            if (st) st.textContent = '提交中…';
            var p = isEdit
                ? {
                      entity: 'node',
                      action: 'update',
                      payload: {
                          node_id: Number(editId.value),
                          node_name: node_name,
                          auth_key: auth_key,
                      },
                  }
                : {
                      entity: 'node',
                      action: 'create',
                      payload: { node_name: node_name, auth_key: auth_key },
                  };
            if (!isEdit && !auth_key) {
                if (st) st.textContent = '新建节点必须填写 auth_key';
                return;
            }
            api.adminMutate(tok, p)
                .then(function () {
                    if (st) {
                        st.textContent = '已保存';
                        st.classList.add('rpc-admin-status--ok');
                    }
                    root.querySelector('#rpc-admin-node-cancel').click();
                    refreshDirectory(root);
                })
                .catch(function (e) {
                    if (st) {
                        st.textContent = (e && e.message) || '失败';
                        st.classList.remove('rpc-admin-status--ok');
                    }
                });
        });

        root.querySelector('#rpc-admin-node-cancel').addEventListener('click', function () {
            var editId = root.querySelector('#rpc-admin-node-edit-id');
            var nameEl = root.querySelector('#rpc-admin-node-name');
            var keyEl = root.querySelector('#rpc-admin-node-key');
            var st = root.querySelector('#rpc-admin-node-form-status');
            if (editId) editId.value = '';
            if (nameEl) nameEl.value = '';
            if (keyEl) keyEl.value = '';
            if (st) {
                st.textContent = '';
                st.classList.remove('rpc-admin-status--ok');
            }
        });

        root.querySelector('#rpc-admin-submit-app').addEventListener('click', function () {
            var st = root.querySelector('#rpc-admin-form-status');
            if (!api || !api.adminMutate) {
                if (st) st.textContent = 'adminMutate 不可用';
                return;
            }
            var hid = root.querySelector('#rpc-admin-app-edit-id');
            var node = root.querySelector('#rpc-admin-in-node');
            var title = root.querySelector('#rpc-admin-in-title');
            var exe = root.querySelector('#rpc-admin-in-exe');
            var icon = root.querySelector('#rpc-admin-in-icon');
            var pub = root.querySelector('#rpc-admin-in-public');
            var payload = {
                node_name: node && node.value ? node.value.trim() : '',
                display_name: title && title.value ? title.value.trim() : '',
                exe_path: exe && exe.value ? exe.value.trim() : '',
                icon_base64: icon && icon.value ? icon.value.trim() : '',
                is_public: pub && pub.checked ? 1 : 0,
            };
            if (!payload.node_name || !payload.display_name || !payload.exe_path) {
                if (st) st.textContent = '请选择采集节点，并填写显示名与 exe 路径';
                return;
            }
            if (st) st.textContent = '提交中…';
            var isEdit = hid && hid.value && String(hid.value).trim().length > 0;
            var req = isEdit
                ? {
                      entity: 'application',
                      action: 'update',
                      payload: Object.assign({ app_id: Number(hid.value) }, payload),
                  }
                : { entity: 'application', action: 'create', payload: payload };
            api.adminMutate(tok, req)
                .then(function () {
                    if (st) {
                        st.textContent = '已保存';
                        st.classList.add('rpc-admin-status--ok');
                    }
                    hid.value = '';
                    if (root.querySelector('#rpc-admin-submit-app'))
                        root.querySelector('#rpc-admin-submit-app').textContent = '保存';
                    syncCatalogAndDesktop(tok, api).then(function () {
                        refreshDirectory(root);
                        refreshStats(root);
                    });
                })
                .catch(function (err) {
                    if (st) {
                        st.textContent = (err && err.message) || '失败';
                        st.classList.remove('rpc-admin-status--ok');
                    }
                });
        });

        (function wireAppIconFile() {
            var iconFile = root.querySelector('#rpc-admin-in-icon-file');
            var iconHid = root.querySelector('#rpc-admin-in-icon');
            var iconName = root.querySelector('#rpc-admin-in-icon-name');
            if (!iconFile || !iconHid) return;
            iconFile.addEventListener('change', function () {
                var f = iconFile.files && iconFile.files[0];
                if (!f) {
                    iconHid.value = '';
                    if (iconName) iconName.textContent = '';
                    return;
                }
                readIconFileAsBase64(
                    f,
                    function (b64) {
                        iconHid.value = b64;
                        if (iconName) iconName.textContent = f.name;
                    },
                    function (msg) {
                        global.alert(msg);
                        iconFile.value = '';
                        iconHid.value = '';
                        if (iconName) iconName.textContent = '';
                    }
                );
            });
        })();

        root.querySelector('#rpc-admin-user-save').addEventListener('click', function () {
            var st = root.querySelector('#rpc-admin-user-form-status');
            if (!api || !api.adminMutate) {
                if (st) st.textContent = 'adminMutate 不可用';
                return;
            }
            var nameEl = root.querySelector('#rpc-admin-user-name');
            var passEl = root.querySelector('#rpc-admin-user-pass');
            var roleEl = root.querySelector('#rpc-admin-user-role');
            var username = nameEl && nameEl.value ? nameEl.value.trim() : '';
            var role = roleEl && roleEl.value ? roleEl.value.trim() : '';
            var plainPass = passEl && passEl.value ? passEl.value : '';
            var editingId = root._rpcAdminUserEditingId;
            var isEdit = editingId != null && typeof editingId === 'number' && editingId > 0;
            if (!username || !role) {
                if (st) st.textContent = '请填写用户名与角色';
                return;
            }
            if (!isEdit && !plainPass) {
                if (st) st.textContent = '新建用户必须填写口令';
                return;
            }
            if (st) st.textContent = '提交中…';
            var pwPayload = plainPass ? pwdTransport(plainPass) : '';
            var req = isEdit
                ? {
                      entity: 'user',
                      action: 'update',
                      payload: {
                          user_id: editingId,
                          username: username,
                          role: role,
                          password: pwPayload,
                      },
                  }
                : {
                      entity: 'user',
                      action: 'create',
                      payload: { username: username, role: role, password: pwPayload },
                  };
            api.adminMutate(tok, req)
                .then(function () {
                    if (st) {
                        st.textContent = '已保存';
                        st.classList.add('rpc-admin-status--ok');
                    }
                    root.querySelector('#rpc-admin-user-cancel').click();
                    refreshDirectory(root);
                })
                .catch(function (e) {
                    if (st) {
                        st.textContent = formatAdminMutateError(e && e.message);
                        st.classList.remove('rpc-admin-status--ok');
                    }
                });
        });

        root.querySelector('#rpc-admin-user-cancel').addEventListener('click', function () {
            resetPeopleUserForm(root);
        });

        root.querySelector('#rpc-admin-user-new').addEventListener('click', function () {
            root.querySelector('#rpc-admin-user-cancel').click();
        });

        root.querySelector('#rpc-admin-perm-save').addEventListener('click', function () {
            var st = root.querySelector('#rpc-admin-perm-form-status');
            if (!api || !api.adminMutate) {
                if (st) st.textContent = 'adminMutate 不可用';
                return;
            }
            var pid = root.querySelector('#rpc-admin-perm-edit-id');
            var su = root.querySelector('#rpc-admin-perm-user');
            var sa = root.querySelector('#rpc-admin-perm-app');
            var user_id = su ? Number(su.value) : 0;
            var app_id = sa ? Number(sa.value) : 0;
            if (!user_id || !app_id) {
                if (st) st.textContent = '请选择用户与应用';
                return;
            }
            if (st) st.textContent = '提交中…';
            var isEdit = pid && pid.value && String(pid.value).trim().length > 0;
            var req = isEdit
                ? {
                      entity: 'permission',
                      action: 'update',
                      payload: {
                          perm_id: Number(pid.value),
                          user_id: user_id,
                          app_id: app_id,
                      },
                  }
                : {
                      entity: 'permission',
                      action: 'create',
                      payload: { user_id: user_id, app_id: app_id },
                  };
            api.adminMutate(tok, req)
                .then(function () {
                    if (st) {
                        st.textContent = '已保存';
                        st.classList.add('rpc-admin-status--ok');
                    }
                    root.querySelector('#rpc-admin-perm-cancel').click();
                    refreshDirectory(root);
                })
                .catch(function (e) {
                    if (st) {
                        st.textContent = (e && e.message) || '失败';
                        st.classList.remove('rpc-admin-status--ok');
                    }
                });
        });

        root.querySelector('#rpc-admin-perm-cancel').addEventListener('click', function () {
            var pid = root.querySelector('#rpc-admin-perm-edit-id');
            var su = root.querySelector('#rpc-admin-perm-user');
            var sa = root.querySelector('#rpc-admin-perm-app');
            if (pid) pid.value = '';
            if (su) su.value = '';
            if (sa) sa.value = '';
            var st = root.querySelector('#rpc-admin-perm-form-status');
            if (st) {
                st.textContent = '';
                st.classList.remove('rpc-admin-status--ok');
            }
        });
    }

    function ensureOverlay(doc) {
        var el = doc.getElementById('rpc-admin-overlay');
        if (el) return el;
        el = doc.createElement('div');
        el.id = 'rpc-admin-overlay';
        el.setAttribute('hidden', '');
        el.innerHTML =
            '<div class="rpc-admin-frame">' +
            '<header class="rpc-admin-head">' +
            '<div class="rpc-admin-head-brand">' +
            '<div class="rpc-admin-head-title">管控台</div>' +
            '</div>' +
            '<div class="rpc-admin-head-spacer"></div>' +
            '<button type="button" class="rpc-admin-icon-btn" id="rpc-admin-close" title="关闭" aria-label="关闭">×</button>' +
            '</header>' +
            '<div class="rpc-admin-body">' +
            '<aside class="rpc-admin-sidebar" aria-label="导航">' +
            '<nav class="rpc-admin-nav">' +
            '<button type="button" class="rpc-admin-nav-item rpc-admin-nav-item--active" data-admin-tab="stats" aria-controls="rpc-admin-page-stats">' +
            '<span class="rpc-admin-nav-ic" aria-hidden="true">▣</span><span class="rpc-admin-nav-txt">统计</span></button>' +
            '<button type="button" class="rpc-admin-nav-item" data-admin-tab="nodes" aria-controls="rpc-admin-page-nodes">' +
            '<span class="rpc-admin-nav-ic" aria-hidden="true">◈</span><span class="rpc-admin-nav-txt">节点</span></button>' +
            '<button type="button" class="rpc-admin-nav-item" data-admin-tab="apps" aria-controls="rpc-admin-page-apps">' +
            '<span class="rpc-admin-nav-ic" aria-hidden="true">◧</span><span class="rpc-admin-nav-txt">应用</span></button>' +
            '<button type="button" class="rpc-admin-nav-item" data-admin-tab="people" aria-controls="rpc-admin-page-people">' +
            '<span class="rpc-admin-nav-ic" aria-hidden="true">◎</span><span class="rpc-admin-nav-txt">人员</span></button>' +
            '</nav></aside>' +
            '<div class="rpc-admin-main">' +
            '<div class="rpc-admin-main-inner">' +
            '<div class="rpc-admin-meta" id="rpc-admin-dir-meta" hidden>' +
            '<span id="rpc-admin-dir-status"></span></div>' +
            '<div class="rpc-admin-panel-wrap">' +
            '<section class="rpc-admin-panel rpc-admin-page" id="rpc-admin-page-stats" data-admin-panel="stats" role="tabpanel" aria-label="统计">' +
            '<div class="rpc-admin-page-hero">' +
            '<div class="rpc-admin-page-hero-text">' +
            '<h2 class="rpc-admin-page-title">统计</h2>' +
            '<p class="rpc-admin-page-lead">在线节点 · 用量 · 会话趋势</p></div>' +
            '<div class="rpc-admin-page-hero-actions">' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-secondary" id="rpc-admin-refresh-stats">刷新</button>' +
            '<span class="rpc-admin-inline-status" id="rpc-admin-fetch-status"></span></div></div>' +
            '<div class="rpc-admin-kpi-row">' +
            '<div class="rpc-admin-kpi"><div class="rpc-admin-kpi-label">活跃会话</div>' +
            '<div class="rpc-admin-kpi-value" id="rpc-admin-kpi-active">—</div></div>' +
            '<div class="rpc-admin-kpi"><div class="rpc-admin-kpi-label">在线节点</div>' +
            '<div class="rpc-admin-kpi-value" id="rpc-admin-kpi-nodes">—</div></div></div>' +
            '<div class="rpc-admin-stats-split">' +
            '<div class="rpc-admin-stats-pane rpc-admin-card">' +
            '<div class="rpc-admin-section-title">在线节点</div>' +
            '<div class="rpc-admin-nodes rpc-admin-nodes--large" id="rpc-admin-nodes"></div></div>' +
            '<div class="rpc-admin-stats-pane rpc-admin-card">' +
            '<div class="rpc-admin-section-title">应用用量</div>' +
            '<div class="rpc-admin-chart-wrap rpc-admin-chart-wrap--tall">' +
            '<div class="rpc-admin-bars rpc-admin-bars--tall" id="rpc-admin-bars"></div></div></div></div>' +
            '<div class="rpc-admin-trend-block rpc-admin-card">' +
            '<div class="rpc-admin-section-title">活跃会话趋势</div>' +
            '<canvas class="rpc-admin-line-canvas rpc-admin-line-canvas--tall" id="rpc-admin-spark" width="1200" height="220"></canvas></div>' +
            '</section>' +
            '<section class="rpc-admin-panel rpc-admin-page" id="rpc-admin-page-nodes" data-admin-panel="nodes" role="tabpanel" aria-label="节点" hidden>' +
            '<div class="rpc-admin-page-hero">' +
            '<div class="rpc-admin-page-hero-text">' +
            '<h2 class="rpc-admin-page-title">节点</h2>' +
            '<p class="rpc-admin-page-lead">采集端登记与鉴权</p></div>' +
            '<div class="rpc-admin-page-hero-actions">' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-secondary" id="rpc-admin-refresh-dir-nodes">刷新</button></div></div>' +
            '<div class="rpc-admin-card rpc-admin-card--form">' +
            '<div class="rpc-admin-section-title">编辑</div>' +
            '<input type="hidden" id="rpc-admin-node-edit-id" value="" />' +
            '<div class="rpc-admin-form-grid">' +
            '<div class="rpc-admin-field"><label>节点名</label>' +
            '<input type="text" id="rpc-admin-node-name" placeholder="与 Agent 一致" /></div>' +
            '<div class="rpc-admin-field"><label>鉴权密钥</label>' +
            '<input type="password" id="rpc-admin-node-key" placeholder="新建必填；编辑留空不改" autocomplete="new-password" /></div></div>' +
            '<div class="rpc-admin-actions">' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-primary" id="rpc-admin-node-save">保存</button>' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-secondary" id="rpc-admin-node-cancel">清空</button></div>' +
            '<div class="rpc-admin-status" id="rpc-admin-node-form-status"></div></div>' +
            '<div class="rpc-admin-section-title rpc-admin-section-title--table">列表</div>' +
            '<div class="rpc-admin-table-scroll rpc-admin-table-scroll--fill">' +
            '<table class="rpc-admin-table">' +
            '<thead><tr><th>节点 ID</th><th>节点名</th><th>在线</th><th>最近上报</th><th>操作</th></tr></thead>' +
            '<tbody id="rpc-admin-tbody-nodes"></tbody></table></div></section>' +
            '<section class="rpc-admin-panel rpc-admin-page rpc-admin-page--standalone" id="rpc-admin-page-apps" data-admin-panel="apps" role="tabpanel" aria-label="应用" hidden>' +
            '<div class="rpc-admin-page-hero">' +
            '<div class="rpc-admin-page-hero-text">' +
            '<h2 class="rpc-admin-page-title">应用</h2>' +
            '<p class="rpc-admin-page-lead">远程应用登记</p></div>' +
            '<div class="rpc-admin-page-hero-actions">' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-secondary" id="rpc-admin-refresh-dir-apps">刷新</button></div></div>' +
            '<div class="rpc-admin-page-body rpc-admin-page-body--split">' +
            '<div class="rpc-admin-apps-stack">' +
            '<div class="rpc-admin-subpage" data-subpage="app-form">' +
            '<div class="rpc-admin-subpage-head">登记</div>' +
            '<input type="hidden" id="rpc-admin-app-edit-id" value="" />' +
            '<div>' +
            '<div class="rpc-admin-form-grid">' +
            '<div class="rpc-admin-field"><label for="rpc-admin-in-node">节点</label>' +
            '<select id="rpc-admin-in-node"></select></div>' +
            '<div class="rpc-admin-field"><label for="rpc-admin-in-title">显示名称</label>' +
            '<input type="text" id="rpc-admin-in-title" placeholder="桌面显示名" /></div>' +
            '<div class="rpc-admin-field" style="grid-column:1/-1"><label for="rpc-admin-in-exe">可执行路径</label>' +
            '<input type="text" id="rpc-admin-in-exe" placeholder="例如 C:\\\\Apps\\\\app.exe" /></div>' +
            '<div class="rpc-admin-field rpc-admin-field--icon-upload" style="grid-column:1/-1">' +
            '<label for="rpc-admin-in-icon-file">图标</label>' +
            '<input type="file" id="rpc-admin-in-icon-file" accept=".ico,.png,.svg,image/png,image/svg+xml,image/x-icon,image/vnd.microsoft.icon" />' +
            '<input type="hidden" id="rpc-admin-in-icon" value="" />' +
            '<span class="rpc-admin-icon-file-name" id="rpc-admin-in-icon-name"></span></div>' +
            '<div class="rpc-admin-field rpc-admin-field--checkbox">' +
            '<label class="rpc-admin-checkbox-label" for="rpc-admin-in-public">' +
            '<input type="checkbox" id="rpc-admin-in-public" /><span>公开应用（所有登录用户可见）</span></label></div>' +
            '</div>' +
            '<div class="rpc-admin-actions">' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-primary" id="rpc-admin-submit-app">保存</button>' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-secondary" id="rpc-admin-app-cancel">清空</button>' +
            '</div>' +
            '<div class="rpc-admin-status" id="rpc-admin-form-status"></div></div></div>' +
            '<div class="rpc-admin-subpage" data-subpage="app-list">' +
            '<div class="rpc-admin-subpage-head">已登记</div>' +
            '<div>' +
            '<div class="rpc-admin-table-scroll rpc-admin-table-scroll--wide">' +
            '<table class="rpc-admin-table">' +
            '<thead><tr><th>应用 ID</th><th>节点</th><th>显示名</th><th>exe_path</th><th>公开</th><th>操作</th></tr></thead>' +
            '<tbody id="rpc-admin-tbody-apps"></tbody></table></div></div></div></div></div></section>' +
            '<section class="rpc-admin-panel rpc-admin-page rpc-admin-page--standalone" id="rpc-admin-page-people" data-admin-panel="people" role="tabpanel" aria-label="人员" hidden>' +
            '<div class="rpc-admin-page-hero">' +
            '<div class="rpc-admin-page-hero-text">' +
            '<h2 class="rpc-admin-page-title">人员</h2>' +
            '<p class="rpc-admin-page-lead">用户与访问授权</p></div>' +
            '<div class="rpc-admin-page-hero-actions">' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-secondary" id="rpc-admin-refresh-dir-people">刷新</button></div></div>' +
            '<div class="rpc-admin-page-body rpc-admin-page-body--split">' +
            '<div class="rpc-admin-people-stack">' +
            '<div class="rpc-admin-crud-block rpc-admin-subpage" data-subpage="users">' +
            '<div class="rpc-admin-subpage-head rpc-admin-subpage-head--split">' +
            '<span>用户</span>' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-ghost" id="rpc-admin-user-new">新建</button></div>' +
            '<p class="rpc-admin-hint-line" id="rpc-admin-user-mode-hint"></p>' +
            '<input type="hidden" id="rpc-admin-user-edit-id" value="" />' +
            '<div class="rpc-admin-form-grid">' +
            '<div class="rpc-admin-field"><label>用户名</label><input type="text" id="rpc-admin-user-name" /></div>' +
            '<div class="rpc-admin-field"><label>口令</label>' +
            '<input type="password" id="rpc-admin-user-pass" placeholder="编辑可留空" autocomplete="new-password" /></div>' +
            '<div class="rpc-admin-field"><label for="rpc-admin-user-role">角色</label>' +
            '<select id="rpc-admin-user-role">' +
            '<option value="user">普通用户</option>' +
            '<option value="admin">管理员</option>' +
            '</select></div></div>' +
            '<div class="rpc-admin-actions">' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-primary" id="rpc-admin-user-save">保存</button>' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-secondary" id="rpc-admin-user-cancel">清空</button></div>' +
            '<div class="rpc-admin-status" id="rpc-admin-user-form-status"></div>' +
            '<div class="rpc-admin-table-scroll rpc-admin-table-scroll--half">' +
            '<table class="rpc-admin-table">' +
            '<thead><tr><th>ID</th><th>用户名</th><th>角色</th><th>操作</th></tr></thead>' +
            '<tbody id="rpc-admin-tbody-users"></tbody></table></div></div>' +
            '<div class="rpc-admin-crud-block rpc-admin-subpage" data-subpage="perms">' +
            '<div class="rpc-admin-subpage-head">授权</div>' +
            '<input type="hidden" id="rpc-admin-perm-edit-id" value="" />' +
            '<div class="rpc-admin-form-grid">' +
            '<div class="rpc-admin-field"><label>用户</label><select id="rpc-admin-perm-user"></select></div>' +
            '<div class="rpc-admin-field"><label>应用</label><select id="rpc-admin-perm-app"></select></div></div>' +
            '<div class="rpc-admin-actions">' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-primary" id="rpc-admin-perm-save">保存</button>' +
            '<button type="button" class="rpc-admin-btn rpc-admin-btn-secondary" id="rpc-admin-perm-cancel">清空</button></div>' +
            '<div class="rpc-admin-status" id="rpc-admin-perm-form-status"></div>' +
            '<div class="rpc-admin-table-scroll rpc-admin-table-scroll--half">' +
            '<table class="rpc-admin-table">' +
            '<thead><tr><th>ID</th><th>用户</th><th>应用</th><th>名称</th><th>节点</th><th>操作</th></tr></thead>' +
            '<tbody id="rpc-admin-tbody-perms"></tbody></table></div></div></div></div>' +
            '</section>' +
            '</div></div></div></div></div>';
        doc.body.appendChild(el);

        el.querySelector('#rpc-admin-close').addEventListener('click', function () {
            stopAuto();
            resetPeopleUserForm(el);
            el.setAttribute('hidden', '');
        });
        el.addEventListener('click', function (ev) {
            if (ev.target === el) {
                stopAuto();
                resetPeopleUserForm(el);
                el.setAttribute('hidden', '');
            }
        });

        el.querySelectorAll('[data-admin-tab]').forEach(function (btn) {
            btn.addEventListener('click', function () {
                setTab(el, btn.getAttribute('data-admin-tab'));
            });
        });

        el.querySelector('#rpc-admin-refresh-stats').addEventListener('click', function () {
            refreshStats(el);
        });
        el.querySelector('#rpc-admin-refresh-dir-nodes').addEventListener('click', function () {
            refreshDirectory(el);
        });
        el.querySelector('#rpc-admin-refresh-dir-apps').addEventListener('click', function () {
            refreshDirectory(el);
        });
        el.querySelector('#rpc-admin-refresh-dir-people').addEventListener('click', function () {
            refreshDirectory(el);
        });

        el.querySelector('#rpc-admin-app-cancel').addEventListener('click', function () {
            var hid = el.querySelector('#rpc-admin-app-edit-id');
            if (hid) hid.value = '';
            var nodeSel = el.querySelector('#rpc-admin-in-node');
            if (nodeSel) nodeSel.value = '';
            ['#rpc-admin-in-title', '#rpc-admin-in-exe', '#rpc-admin-in-icon'].forEach(function (s) {
                var x = el.querySelector(s);
                if (x) x.value = '';
            });
            var iconFile = el.querySelector('#rpc-admin-in-icon-file');
            if (iconFile) iconFile.value = '';
            var iconName = el.querySelector('#rpc-admin-in-icon-name');
            if (iconName) iconName.textContent = '';
            var pub = el.querySelector('#rpc-admin-in-public');
            if (pub) pub.checked = false;
            var sub = el.querySelector('#rpc-admin-submit-app');
            if (sub) sub.textContent = '保存';
            var st = el.querySelector('#rpc-admin-form-status');
            if (st) {
                st.textContent = '';
                st.classList.remove('rpc-admin-status--ok');
            }
        });

        wireCrudForms(el);
        setPeopleUserEditingId(el, null);
        syncUserFormModeHint(el);
        return el;
    }

    global.__rpcOpenAdminConsole = function (doc) {
        if (!doc) doc = document;
        if (String(global.__rpcAuthRole || '').toLowerCase() !== 'admin') {
            global.alert('当前账号非管理员，无法打开管控台。');
            return;
        }
        var el = ensureOverlay(doc);
        resetPeopleUserForm(el);
        el.removeAttribute('hidden');
        setTab(el, 'stats');
        stopAuto();
        autoTimer = setInterval(function () {
            if (el.hasAttribute('hidden')) return;
            if (getActiveTabKey(el) === 'stats') refreshStats(el);
        }, 12000);
    };
})(typeof window !== 'undefined' ? window : this);
