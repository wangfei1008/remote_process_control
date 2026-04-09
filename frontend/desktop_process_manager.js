/**
 * 桌面模式：系统设置 → 进程管理
 * - 全局状态（zustand-like）：window.__rpcDesktopAppStore
 * - 持久化：localStorage（rpc_desktop_app_entries_v2）
 * - UI：右侧 400px 滑出面板 + 半透明遮罩
 *
 * AppEntry: { id, name, icon, workPath, workNode }
 */
(function () {
    'use strict';

    var STORAGE_V2 = 'rpc_desktop_app_entries_v2';
    var STORAGE_V1 = 'rpc_desktop_app_entries_v1';

    var DEFAULT_ENTRIES = [
        { id: '1', name: '文件管理器', icon: '📁', workPath: '/home/user', workNode: '本地主机' },
        { id: '2', name: '终端', icon: '>_', workPath: '/bin/bash', workNode: '开发节点-01' },
    ];

    function generateEntryId() {
        return 'app_' + window.__rpcRandomId(12);
    }

    function normalizeEntry(e) {
        if (!e || typeof e !== 'object') return null;
        return {
            id: String(e.id || generateEntryId()),
            name: String(e.name || '').trim(),
            icon: String(e.icon || '⬚').trim() || '⬚',
            workPath: String(e.workPath || '').trim(),
            workNode: String(e.workNode || '').trim(),
        };
    }

    function validateName(name) {
        var s = String(name || '').trim();
        if (!s || s.length < 1 || s.length > 20) return '名称长度须为 1–20';
        return '';
    }

    function validateWorkPath(p) {
        var s = String(p || '').trim();
        if (!s) return '工作路径不能为空';
        return '';
    }

    function validateWorkNode(n) {
        var s = String(n || '').trim();
        if (!s || s.length < 1 || s.length > 30) return '工作节点名称长度须为 1–30';
        return '';
    }

    function validateIcon(v) {
        var s = String(v || '').trim();
        if (!s) return '图标不能为空';
        if (/^https?:\/\//i.test(s) || /^data:image\//i.test(s)) return '';
        // 允许 emoji / 短文本
        if (s.length > 8) return '图标过长（建议使用 emoji 或图片 URL）';
        return '';
    }

    function safeParse(raw) {
        try { return JSON.parse(raw); } catch (_) { return null; }
    }

    function loadFromStorageOrDefault() {
        try {
            var raw2 = localStorage.getItem(STORAGE_V2);
            if (raw2) {
                var p2 = safeParse(raw2);
                if (Array.isArray(p2)) {
                    var ok2 = p2.map(normalizeEntry).filter(function (x) { return x && x.id && x.name; });
                    if (ok2.length) return ok2;
                }
            }
        } catch (_) {}

        // migrate v1 -> v2
        try {
            var raw1 = localStorage.getItem(STORAGE_V1);
            if (raw1) {
                var p1 = safeParse(raw1);
                if (Array.isArray(p1) && p1.length) {
                    var migrated = p1.map(function (x) {
                        var n = normalizeEntry({
                            id: x.id,
                            name: x.name,
                            icon: x.icon,
                            workPath: x.workPath || x.exePath,
                            workNode: '本地主机',
                        });
                        return n;
                    }).filter(function (x) { return x && x.id && x.name; });
                    if (migrated.length) {
                        try { localStorage.setItem(STORAGE_V2, JSON.stringify(migrated)); } catch (_) {}
                        return migrated;
                    }
                }
            }
        } catch (_) {}

        return DEFAULT_ENTRIES.map(normalizeEntry);
    }

    function persist(entries) {
        try { localStorage.setItem(STORAGE_V2, JSON.stringify(entries)); } catch (_) {}
    }

    // zustand-like store
    (function initStore(global) {
        if (global.__rpcDesktopAppStore) return;
        var state = { processes: loadFromStorageOrDefault() };
        var listeners = [];
        global.__rpcDesktopAppStore = {
            getState: function () { return { processes: state.processes.slice() }; },
            setState: function (next) {
                if (!next || !Array.isArray(next.processes)) return;
                state.processes = next.processes.slice();
                persist(state.processes);
                listeners.slice().forEach(function (fn) {
                    try { fn({ processes: state.processes.slice() }); } catch (_) {}
                });
            },
            subscribe: function (fn) {
                if (typeof fn !== 'function') return function () {};
                listeners.push(fn);
                return function () {
                    var i = listeners.indexOf(fn);
                    if (i >= 0) listeners.splice(i, 1);
                };
            },
        };
    })(window);

    var PRESET_ICONS = ['📁', '💻', '🖥', '⚙', '🌐', '🗂', '📄', '📝', '>_', '⬚'];

    function mountPanel(opts) {
        var doc = opts.doc;
        var desktopRoot = opts.desktopRoot;

        var overlay = doc.createElement('div');
        overlay.className = 'rpc-dpm-overlay';
        var panel = doc.createElement('div');
        panel.className = 'rpc-dpm-panel';

        var header = doc.createElement('div');
        header.className = 'rpc-dpm-header';
        var headerText = doc.createElement('div');
        headerText.className = 'rpc-dpm-header-text';
        var breadcrumb = doc.createElement('div');
        breadcrumb.className = 'rpc-dpm-breadcrumb';
        breadcrumb.textContent = '系统设置';
        var title = doc.createElement('div');
        title.className = 'rpc-dpm-title';
        title.textContent = '进程管理';
        headerText.appendChild(breadcrumb);
        headerText.appendChild(title);
        var btnClose = doc.createElement('button');
        btnClose.type = 'button';
        btnClose.className = 'rpc-dpm-close';
        btnClose.textContent = '✕';
        header.appendChild(headerText);
        header.appendChild(btnClose);

        var toolbar = doc.createElement('div');
        toolbar.className = 'rpc-dpm-toolbar';
        var btnAdd = doc.createElement('button');
        btnAdd.type = 'button';
        btnAdd.className = 'rpc-dpm-btn rpc-dpm-btn--primary';
        btnAdd.textContent = '+ 添加';
        toolbar.appendChild(btnAdd);

        var listWrap = doc.createElement('div');
        listWrap.className = 'rpc-dpm-list-wrap';

        var formWrap = doc.createElement('div');
        formWrap.className = 'rpc-dpm-form-wrap';

        function label(text) {
            var el = doc.createElement('div');
            el.className = 'rpc-dpm-label';
            el.textContent = text;
            return el;
        }

        function input(placeholder) {
            var el = doc.createElement('input');
            el.type = 'text';
            el.placeholder = placeholder || '';
            el.className = 'rpc-dpm-input';
            return el;
        }

        var inpName = input('名称（1-20）');
        var inpIconUrl = input('图标 URL（可选）');
        var inpWorkPath = input('工作路径');
        var inpWorkNode = input('工作节点名称（1-30）');

        var presets = doc.createElement('div');
        presets.className = 'rpc-dpm-presets';
        var selectedPreset = PRESET_ICONS[0];

        var preview = doc.createElement('div');
        preview.className = 'rpc-dpm-icon-preview';
        preview.textContent = selectedPreset;

        var err = doc.createElement('div');
        err.className = 'rpc-dpm-error';

        var actions = doc.createElement('div');
        actions.className = 'rpc-dpm-form-actions';
        var btnSave = doc.createElement('button');
        btnSave.type = 'button';
        btnSave.className = 'rpc-dpm-btn rpc-dpm-btn--primary';
        btnSave.textContent = '保存';
        var btnCancel = doc.createElement('button');
        btnCancel.type = 'button';
        btnCancel.className = 'rpc-dpm-btn rpc-dpm-btn--ghost';
        btnCancel.textContent = '取消';
        actions.appendChild(btnSave);
        actions.appendChild(btnCancel);

        PRESET_ICONS.forEach(function (em) {
            var b = doc.createElement('button');
            b.type = 'button';
            b.className = 'rpc-dpm-preset-btn';
            b.textContent = em;
            b.addEventListener('click', function () {
                selectedPreset = em;
                inpIconUrl.value = '';
                preview.innerHTML = '';
                preview.textContent = em;
            });
            presets.appendChild(b);
        });

        formWrap.appendChild(label('名称（必填）'));
        formWrap.appendChild(inpName);
        formWrap.appendChild(label('图标（URL 或预设，必填）'));
        formWrap.appendChild(presets);
        formWrap.appendChild(inpIconUrl);
        formWrap.appendChild(preview);
        formWrap.appendChild(label('工作路径（必填）'));
        formWrap.appendChild(inpWorkPath);
        formWrap.appendChild(label('工作节点名称（必填）'));
        formWrap.appendChild(inpWorkNode);
        formWrap.appendChild(err);
        formWrap.appendChild(actions);

        panel.appendChild(header);
        panel.appendChild(toolbar);
        panel.appendChild(listWrap);
        panel.appendChild(formWrap);
        desktopRoot.appendChild(overlay);
        desktopRoot.appendChild(panel);

        var editingId = null;

        function readIconValue() {
            var url = String(inpIconUrl.value || '').trim();
            if (url) return url;
            return selectedPreset || '⬚';
        }

        function setPreviewFromUrl(url) {
            preview.innerHTML = '';
            if (!url) {
                preview.textContent = selectedPreset || '⬚';
                return;
            }
            if (/^https?:\/\//i.test(url) || /^data:image\//i.test(url)) {
                var img = doc.createElement('img');
                img.src = url;
                img.onerror = function () {
                    preview.innerHTML = '';
                    preview.textContent = '⬚';
                };
                preview.appendChild(img);
            } else {
                preview.textContent = url;
            }
        }

        inpIconUrl.addEventListener('input', function () {
            setPreviewFromUrl(String(inpIconUrl.value || '').trim());
        });

        function showForm(entry) {
            formWrap.classList.add('rpc-dpm-form-wrap--open');
            panel.classList.add('rpc-dpm-panel--editing');
            editingId = entry ? entry.id : null;
            inpName.value = entry ? entry.name : '';
            inpWorkPath.value = entry ? entry.workPath : '';
            inpWorkNode.value = entry ? entry.workNode : '';
            inpIconUrl.value = (entry && entry.icon && (/^https?:\/\//i.test(entry.icon) || /^data:image\//i.test(entry.icon)))
                ? entry.icon
                : '';
            selectedPreset = (!inpIconUrl.value && entry && entry.icon) ? entry.icon : (selectedPreset || '⬚');
            err.textContent = '';
            setPreviewFromUrl(readIconValue());
        }

        function hideForm() {
            formWrap.classList.remove('rpc-dpm-form-wrap--open');
            panel.classList.remove('rpc-dpm-panel--editing');
            editingId = null;
            err.textContent = '';
        }

        function iconCell(icon) {
            if (icon && (/^https?:\/\//i.test(icon) || /^data:image\//i.test(icon))) {
                return '<img alt="" src="' + String(icon).replace(/"/g, '&quot;') + '"/>';
            }
            return String(icon || '⬚');
        }

        function renderList() {
            var entries = window.__rpcDesktopAppStore.getState().processes.slice();
            if (!entries.length) {
                listWrap.innerHTML = '<div class="rpc-dpm-empty">暂无进程，请点击「添加」。</div>';
                return;
            }
            var html = '';
            html += '<table class="rpc-dpm-table">';
            html += '<thead><tr>';
            html += '<th style="width:72px;">图标</th>';
            html += '<th>名称</th>';
            html += '<th>工作路径</th>';
            html += '<th>工作节点名称</th>';
            html += '<th style="width:120px;">操作</th>';
            html += '</tr></thead>';
            html += '<tbody>';
            entries.forEach(function (e) {
                html += '<tr data-id="' + String(e.id).replace(/"/g, '&quot;') + '">';
                html += '<td><div class="rpc-dpm-table-icon">' + iconCell(e.icon) + '</div></td>';
                html += '<td class="rpc-dpm-td-ellipsis" title="' + String(e.name).replace(/"/g, '&quot;') + '">' + String(e.name) + '</td>';
                html += '<td class="rpc-dpm-td-ellipsis" title="' + String(e.workPath).replace(/"/g, '&quot;') + '">' + String(e.workPath) + '</td>';
                html += '<td class="rpc-dpm-td-ellipsis" title="' + String(e.workNode).replace(/"/g, '&quot;') + '">' + String(e.workNode) + '</td>';
                html += '<td><div class="rpc-dpm-td-actions">';
                html += '<button type="button" class="rpc-dpm-inline-btn" data-act="edit">编辑</button>';
                html += '<button type="button" class="rpc-dpm-inline-btn rpc-dpm-inline-btn--danger" data-act="del">删除</button>';
                html += '</div></td>';
                html += '</tr>';
            });
            html += '</tbody></table>';
            listWrap.innerHTML = html;

            // bind actions
            Array.from(listWrap.querySelectorAll('button[data-act]')).forEach(function (b) {
                b.addEventListener('click', function () {
                    var tr = b.closest('tr');
                    if (!tr) return;
                    var id = tr.getAttribute('data-id');
                    var list = window.__rpcDesktopAppStore.getState().processes.slice();
                    var cur = list.find(function (x) { return x.id === id; });
                    var act = b.getAttribute('data-act');
                    if (act === 'edit' && cur) {
                        showForm(cur);
                        return;
                    }
                    if (act === 'del' && cur) {
                        if (!window.confirm('确定删除「' + cur.name + '」？')) return;
                        var next = list.filter(function (x) { return x.id !== id; });
                        window.__rpcDesktopAppStore.setState({ processes: next });
                        renderList();
                        hideForm();
                    }
                });
            });
        }

        btnAdd.addEventListener('click', function () { showForm(null); });
        btnCancel.addEventListener('click', hideForm);
        btnSave.addEventListener('click', function () {
            err.textContent = '';
            var name = inpName.value;
            var icon = readIconValue();
            var workPath = inpWorkPath.value;
            var workNode = inpWorkNode.value;
            var e1 = validateName(name) || validateIcon(icon) || validateWorkPath(workPath) || validateWorkNode(workNode);
            if (e1) {
                err.textContent = e1;
                return;
            }
            var entry = normalizeEntry({
                id: editingId || generateEntryId(),
                name: String(name).trim(),
                icon: String(icon).trim(),
                workPath: String(workPath).trim(),
                workNode: String(workNode).trim(),
            });
            var list = window.__rpcDesktopAppStore.getState().processes.slice();
            if (editingId) {
                var ix = list.findIndex(function (x) { return x.id === editingId; });
                if (ix >= 0) list[ix] = entry;
                else list.push(entry);
            } else {
                list.push(entry);
            }
            window.__rpcDesktopAppStore.setState({ processes: list });
            renderList();
            hideForm();
        });

        function open() {
            overlay.classList.add('rpc-dpm-overlay--visible');
            panel.classList.add('rpc-dpm-panel--open');
            renderList();
            hideForm();
        }
        function close() {
            overlay.classList.remove('rpc-dpm-overlay--visible');
            panel.classList.remove('rpc-dpm-panel--open');
            hideForm();
        }

        btnClose.addEventListener('click', close);
        overlay.addEventListener('click', close);
        panel.addEventListener('click', function (e) { e.stopPropagation(); });

        // auto refresh
        var unsub = window.__rpcDesktopAppStore.subscribe(function () {
            renderList();
        });

        return { open: open, close: close, dispose: unsub, refreshList: renderList };
    }

    function loadEntries() {
        return window.__rpcDesktopAppStore.getState().processes.slice();
    }

    function saveEntries(entries) {
        window.__rpcDesktopAppStore.setState({ processes: entries.slice() });
    }

    window.__rpcDesktopProcessManager = {
        STORAGE_V2: STORAGE_V2,
        STORAGE_V1: STORAGE_V1,
        DEFAULT_ENTRIES: DEFAULT_ENTRIES.slice(),
        loadEntries: loadEntries,
        saveEntries: saveEntries,
        normalizeEntry: normalizeEntry,
        validateName: validateName,
        validateWorkPath: validateWorkPath,
        validateWorkNode: validateWorkNode,
        validateIcon: validateIcon,
        mountPanel: mountPanel,
        generateEntryId: generateEntryId,
    };
})();
