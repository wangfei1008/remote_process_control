/**
 * 桌面模式：启动进程管理（localStorage 持久化 + 右侧滑出面板）
 * 依赖：先于 desktopMode.js 加载
 *
 * AppEntry: { id, name, icon, workPath, executable? }
 */
(function () {
    'use strict';

    var STORAGE_KEY = 'rpc_desktop_app_entries_v1';

    function generateEntryId() {
        return 'app_' + window.__rpcRandomId(12);
    }

    function resolveExePath(entry) {
        if (!entry) return '';
        if (entry.exePath) return String(entry.exePath).trim();
        var wp = String(entry.workPath || '').trim();
        var ex = String(entry.executable || '').trim();
        if (!wp) return '';
        if (ex) {
            var sep = wp.indexOf('\\') >= 0 ? '\\' : '/';
            return wp.replace(/[/\\]+$/, '') + sep + ex;
        }
        return wp;
    }

    function isProbablyRunnablePath(s) {
        if (!s) return false;
        return /\.(exe|bat|cmd)$/i.test(s);
    }

    function migrateFromDom(doc) {
        var tiles = Array.from(doc.querySelectorAll('.app-launch-tile[data-rpc-exe]'));
        var out = tiles.map(function (el) {
            var exePath = el.getAttribute('data-rpc-exe') || '';
            var labelEl = el.querySelector('.tile-label');
            var iconEl = el.querySelector('.tile-icon');
            return {
                id: generateEntryId(),
                name: (labelEl && labelEl.textContent.trim()) || exePath,
                icon: (iconEl && iconEl.textContent.trim()) || '⬚',
                workPath: exePath,
            };
        }).filter(function (a) { return a.workPath; });
        if (!out.length) {
            out.push({
                id: generateEntryId(),
                name: '记事本',
                icon: '📝',
                workPath: 'C:\\Windows\\System32\\notepad.exe',
            });
        }
        try {
            localStorage.setItem(STORAGE_KEY, JSON.stringify(out));
        } catch (e) {}
        return out;
    }

    function loadEntries(doc) {
        try {
            var raw = localStorage.getItem(STORAGE_KEY);
            if (raw) {
                var parsed = JSON.parse(raw);
                if (Array.isArray(parsed) && parsed.length) {
                    return parsed.map(normalizeEntry).filter(function (e) { return e && e.id && e.name; });
                }
            }
        } catch (e) {}
        return migrateFromDom(doc);
    }

    function saveEntries(entries) {
        try {
            localStorage.setItem(STORAGE_KEY, JSON.stringify(entries));
        } catch (e) {}
    }

    function normalizeEntry(e) {
        if (!e || typeof e !== 'object') return null;
        return {
            id: String(e.id || generateEntryId()),
            name: String(e.name || '').trim(),
            icon: String(e.icon || '⬚').trim() || '⬚',
            workPath: String(e.workPath || '').trim(),
            executable: e.executable != null ? String(e.executable).trim() : '',
        };
    }

    function validateName(name) {
        var s = String(name || '').trim();
        if (!s || s.length < 1 || s.length > 20) return '名称须为 1–20 个字符';
        return '';
    }

    function validateWorkPath(p) {
        var s = String(p || '').trim();
        if (!s) return '工作路径不能为空';
        if (s.indexOf('\n') >= 0 || s.indexOf('\r') >= 0) return '路径不能包含换行';
        // 简单路径：盘符、UNC、绝对 Unix、~、或含路径分隔符
        var ok = /^([A-Za-z]:[\\/]|\\\\|\/|~[\\/]|[A-Za-z]:[^\\/])/i.test(s) ||
            (/[\\/]/.test(s) && s.length >= 3);
        if (!ok) return '路径格式示例：C:\\\\Users\\\\... 或 /home/user/...';
        return '';
    }

    var PRESET_ICONS = ['📝', '🎨', '🖥', '🗂', '⚙', '🌐', '📁', '💻', '📄', '⬚'];

    /**
     * @param {object} opts
     * @param {Document} opts.doc
     * @param {HTMLElement} opts.desktopRoot
     * @param {function(): object[]} opts.getEntries
     * @param {function(object[]): void} opts.setEntriesAndRefresh
     */
    function mountPanel(opts) {
        var doc = opts.doc;
        var desktopRoot = opts.desktopRoot;
        var getEntries = opts.getEntries;
        var setEntriesAndRefresh = opts.setEntriesAndRefresh;

        var overlay = doc.createElement('div');
        overlay.className = 'rpc-dpm-overlay';
        overlay.style.cssText = [
            'display:none', 'position:fixed', 'inset:0', 'background:rgba(0,0,0,0.45)',
            'z-index:6000', 'opacity:0', 'transition:opacity 0.2s ease',
        ].join(';');

        var panel = doc.createElement('div');
        panel.className = 'rpc-dpm-panel';
        panel.style.cssText = [
            'position:fixed', 'top:0', 'right:0', 'width:min(420px,100vw)', 'height:100%',
            'background:#121a2a', 'box-shadow:-8px 0 40px rgba(0,0,0,0.5)',
            'z-index:6001', 'transform:translateX(100%)', 'transition:transform 0.25s ease',
            'display:flex', 'flex-direction:column', 'border-left:1px solid rgba(255,255,255,0.08)',
        ].join(';');

        var header = doc.createElement('div');
        header.style.cssText = 'display:flex;align-items:center;justify-content:space-between;padding:14px 16px;border-bottom:1px solid rgba(255,255,255,0.08);';
        var hTitle = doc.createElement('div');
        hTitle.textContent = '启动进程管理';
        hTitle.style.cssText = 'font-size:15px;font-weight:700;color:#e7eefc;';
        var btnClose = doc.createElement('button');
        btnClose.type = 'button';
        btnClose.textContent = '✕';
        btnClose.style.cssText = 'width:32px;height:32px;border-radius:8px;border:1px solid rgba(255,255,255,0.12);background:#1a2233;color:#e7eefc;cursor:pointer;font-size:16px;line-height:1;';
        header.appendChild(hTitle);
        header.appendChild(btnClose);

        var toolbar = doc.createElement('div');
        toolbar.style.cssText = 'padding:12px 16px;display:flex;gap:8px;flex-wrap:wrap;align-items:center;';
        var btnAdd = doc.createElement('button');
        btnAdd.type = 'button';
        btnAdd.textContent = '+ 添加进程';
        btnAdd.style.cssText = 'padding:8px 14px;border-radius:10px;border:1px solid rgba(120,160,255,0.4);background:rgba(60,90,200,0.35);color:#dbe4ff;cursor:pointer;font-weight:600;font-size:12px;';
        toolbar.appendChild(btnAdd);

        var listWrap = doc.createElement('div');
        listWrap.style.cssText = 'flex:1;overflow:auto;padding:8px 16px 16px;';

        var formWrap = doc.createElement('div');
        formWrap.style.cssText = 'display:none;border-top:1px solid rgba(255,255,255,0.08);padding:14px 16px;background:rgba(0,0,0,0.2);';

        function elLabel(text) {
            var l = doc.createElement('label');
            l.textContent = text;
            l.style.cssText = 'display:block;font-size:11px;color:rgba(231,238,252,0.75);margin:10px 0 4px;';
            return l;
        }
        function elInput(placeholder) {
            var inp = doc.createElement('input');
            inp.type = 'text';
            inp.placeholder = placeholder || '';
            inp.style.cssText = 'width:100%;box-sizing:border-box;padding:8px 10px;border-radius:8px;border:1px solid rgba(255,255,255,0.12);background:#0d1424;color:#e7eefc;font-size:13px;';
            return inp;
        }
        var inpName = elInput('显示名称');
        var inpWork = elInput('C:\\\\Users\\\\... 或 /home/user/projects');
        var inpExe = elInput('可选，如 notepad.exe');
        var inpIconUrl = elInput('图标 URL（可选）');
        var presetRow = doc.createElement('div');
        presetRow.style.cssText = 'display:flex;flex-wrap:wrap;gap:6px;margin-top:6px;';
        var selectedPreset = '⬚';
        var fileInput = doc.createElement('input');
        fileInput.type = 'file';
        fileInput.accept = 'image/*';
        fileInput.style.cssText = 'margin-top:8px;font-size:11px;color:#9cf;';

        var iconPreview = doc.createElement('div');
        iconPreview.style.cssText = 'width:56px;height:56px;border-radius:12px;background:rgba(255,255,255,0.06);border:1px solid rgba(255,255,255,0.1);display:flex;align-items:center;justify-content:center;font-size:28px;margin-top:8px;overflow:hidden;';
        iconPreview.textContent = '⬚';

        var errBox = doc.createElement('div');
        errBox.style.cssText = 'color:#f88;font-size:12px;margin-top:8px;min-height:18px;';

        var formActions = doc.createElement('div');
        formActions.style.cssText = 'display:flex;gap:8px;margin-top:14px;flex-wrap:wrap;';
        var btnSave = doc.createElement('button');
        btnSave.type = 'button';
        btnSave.textContent = '保存';
        btnSave.style.cssText = 'padding:8px 16px;border-radius:10px;border:0;background:#3b5bdb;color:#fff;cursor:pointer;font-weight:600;font-size:12px;';
        var btnCancelForm = doc.createElement('button');
        btnCancelForm.type = 'button';
        btnCancelForm.textContent = '取消';
        btnCancelForm.style.cssText = 'padding:8px 16px;border-radius:10px;border:1px solid rgba(255,255,255,0.15);background:transparent;color:#e7eefc;cursor:pointer;font-size:12px;';
        formActions.appendChild(btnSave);
        formActions.appendChild(btnCancelForm);

        var editingId = null;
        var iconMode = 'preset'; // preset | url | file

        PRESET_ICONS.forEach(function (em) {
            var b = doc.createElement('button');
            b.type = 'button';
            b.textContent = em;
            b.style.cssText = 'width:36px;height:36px;border-radius:8px;border:1px solid rgba(255,255,255,0.12);background:#1a2233;cursor:pointer;font-size:18px;';
            b.addEventListener('click', function () {
                selectedPreset = em;
                iconMode = 'preset';
                inpIconUrl.value = '';
                fileInput.value = '';
                iconPreview.innerHTML = '';
                iconPreview.textContent = em;
            });
            presetRow.appendChild(b);
        });

        formWrap.appendChild(elLabel('名称（必填）'));
        formWrap.appendChild(inpName);
        formWrap.appendChild(elLabel('图标（预设 / URL / 上传）'));
        formWrap.appendChild(presetRow);
        formWrap.appendChild(inpIconUrl);
        formWrap.appendChild(fileInput);
        formWrap.appendChild(iconPreview);
        formWrap.appendChild(elLabel('工作路径（必填）'));
        formWrap.appendChild(inpWork);
        formWrap.appendChild(elLabel('可执行文件名（可选）'));
        formWrap.appendChild(inpExe);
        formWrap.appendChild(errBox);
        formWrap.appendChild(formActions);

        panel.appendChild(header);
        panel.appendChild(toolbar);
        panel.appendChild(listWrap);
        panel.appendChild(formWrap);

        desktopRoot.appendChild(overlay);
        desktopRoot.appendChild(panel);

        function setIconPreviewFromUrl(url) {
            iconPreview.innerHTML = '';
            if (!url) {
                iconPreview.textContent = selectedPreset || '⬚';
                return;
            }
            if (/^https?:\/\//i.test(url) || /^data:image\//i.test(url)) {
                var img = doc.createElement('img');
                img.src = url;
                img.style.cssText = 'max-width:100%;max-height:100%;object-fit:contain;';
                img.onerror = function () {
                    iconPreview.textContent = '⬚';
                };
                iconPreview.appendChild(img);
            } else {
                iconPreview.textContent = url.length > 4 ? url.slice(0, 4) : url;
            }
        }

        inpIconUrl.addEventListener('input', function () {
            var v = inpIconUrl.value.trim();
            if (v) {
                iconMode = 'url';
                setIconPreviewFromUrl(v);
            } else {
                iconMode = 'preset';
                iconPreview.innerHTML = '';
                iconPreview.textContent = selectedPreset;
            }
        });

        fileInput.addEventListener('change', function () {
            var f = fileInput.files && fileInput.files[0];
            if (!f) return;
            var r = new FileReader();
            r.onload = function () {
                iconMode = 'file';
                inpIconUrl.value = '';
                setIconPreviewFromUrl(r.result);
            };
            r.readAsDataURL(f);
        });

        function readIconValue() {
            if (iconMode === 'file' && iconPreview.querySelector('img')) {
                var im = iconPreview.querySelector('img');
                return im ? im.src : selectedPreset;
            }
            if (iconMode === 'url' && inpIconUrl.value.trim()) return inpIconUrl.value.trim();
            return selectedPreset || '⬚';
        }

        function hideForm() {
            formWrap.style.display = 'none';
            editingId = null;
            errBox.textContent = '';
        }

        function showForm(entry) {
            formWrap.style.display = 'block';
            editingId = entry ? entry.id : null;
            inpName.value = entry ? entry.name : '';
            inpWork.value = entry ? entry.workPath : '';
            inpExe.value = entry && entry.executable ? entry.executable : '';
            inpIconUrl.value = '';
            fileInput.value = '';
            if (entry && entry.icon && (/^https?:\/\//i.test(entry.icon) || /^data:image\//i.test(entry.icon))) {
                iconMode = 'url';
                inpIconUrl.value = entry.icon;
                setIconPreviewFromUrl(entry.icon);
            } else {
                iconMode = 'preset';
                selectedPreset = (entry && entry.icon) ? entry.icon : '⬚';
                if (PRESET_ICONS.indexOf(selectedPreset) < 0) selectedPreset = '⬚';
                iconPreview.innerHTML = '';
                iconPreview.textContent = selectedPreset;
            }
            errBox.textContent = '';
        }

        function renderList() {
            listWrap.innerHTML = '';
            var entries = getEntries();
            if (!entries.length) {
                var empty = doc.createElement('div');
                empty.textContent = '暂无进程，请点击「添加进程」。';
                empty.style.cssText = 'color:rgba(231,238,252,0.5);font-size:13px;padding:20px 8px;text-align:center;';
                listWrap.appendChild(empty);
                return;
            }
            entries.forEach(function (e) {
                var card = doc.createElement('div');
                card.style.cssText = 'display:flex;align-items:center;gap:12px;padding:10px 12px;margin-bottom:8px;border-radius:12px;background:rgba(255,255,255,0.04);border:1px solid rgba(255,255,255,0.08);';

                var thumb = doc.createElement('div');
                thumb.style.cssText = 'width:44px;height:44px;border-radius:10px;flex-shrink:0;background:rgba(0,0,0,0.25);display:flex;align-items:center;justify-content:center;font-size:22px;overflow:hidden;';
                if (e.icon && (/^https?:\/\//i.test(e.icon) || /^data:image\//i.test(e.icon))) {
                    var im = doc.createElement('img');
                    im.src = e.icon;
                    im.style.cssText = 'width:100%;height:100%;object-fit:cover;';
                    im.onerror = function () { thumb.textContent = '⬚'; };
                    thumb.appendChild(im);
                } else {
                    thumb.textContent = e.icon || '⬚';
                }

                var mid = doc.createElement('div');
                mid.style.cssText = 'flex:1;min-width:0;';
                var t = doc.createElement('div');
                t.textContent = e.name;
                t.style.cssText = 'font-weight:700;color:#e7eefc;font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;';
                var p = doc.createElement('div');
                p.textContent = e.workPath + (e.executable ? ' · ' + e.executable : '');
                p.style.cssText = 'font-size:11px;color:rgba(231,238,252,0.55);margin-top:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;';
                mid.appendChild(t);
                mid.appendChild(p);

                var actions = doc.createElement('div');
                actions.style.cssText = 'display:flex;flex-direction:column;gap:4px;flex-shrink:0;';
                var bEdit = doc.createElement('button');
                bEdit.type = 'button';
                bEdit.textContent = '编辑';
                bEdit.style.cssText = 'padding:4px 10px;font-size:11px;border-radius:8px;border:1px solid rgba(255,255,255,0.15);background:#1a2233;color:#cfe0ff;cursor:pointer;';
                var bDel = doc.createElement('button');
                bDel.type = 'button';
                bDel.textContent = '删除';
                bDel.style.cssText = 'padding:4px 10px;font-size:11px;border-radius:8px;border:1px solid rgba(255,100,100,0.35);background:rgba(50,20,20,0.6);color:#fcc;cursor:pointer;';
                actions.appendChild(bEdit);
                actions.appendChild(bDel);

                bEdit.addEventListener('click', function () {
                    showForm(e);
                });
                bDel.addEventListener('click', function () {
                    if (!window.confirm('确定删除「' + e.name + '」？')) return;
                    var next = getEntries().filter(function (x) { return x.id !== e.id; });
                    setEntriesAndRefresh(next);
                    renderList();
                    hideForm();
                });

                card.appendChild(thumb);
                card.appendChild(mid);
                card.appendChild(actions);
                listWrap.appendChild(card);
            });
        }

        btnAdd.addEventListener('click', function () {
            showForm(null);
        });

        btnCancelForm.addEventListener('click', hideForm);

        btnSave.addEventListener('click', function () {
            errBox.textContent = '';
            var name = inpName.value;
            var en = validateName(name);
            if (en) {
                errBox.textContent = en;
                return;
            }
            var wp = inpWork.value;
            var ew = validateWorkPath(wp);
            if (ew) {
                errBox.textContent = ew;
                return;
            }
            var iconVal = readIconValue();
            var exeOpt = inpExe.value.trim();
            var entry = normalizeEntry({
                id: editingId || generateEntryId(),
                name: name.trim(),
                icon: iconVal || '⬚',
                workPath: wp.trim(),
                executable: exeOpt || undefined,
            });
            var list = getEntries().slice();
            if (editingId) {
                var ix = list.findIndex(function (x) { return x.id === editingId; });
                if (ix >= 0) list[ix] = entry;
                else list.push(entry);
            } else {
                list.push(entry);
            }
            setEntriesAndRefresh(list);
            renderList();
            hideForm();
        });

        function openPanel() {
            overlay.style.display = 'block';
            requestAnimationFrame(function () {
                overlay.style.opacity = '1';
                panel.style.transform = 'translateX(0)';
            });
            renderList();
            hideForm();
        }

        function closePanel() {
            overlay.style.opacity = '0';
            panel.style.transform = 'translateX(100%)';
            setTimeout(function () {
                overlay.style.display = 'none';
                hideForm();
            }, 260);
        }

        btnClose.addEventListener('click', closePanel);
        overlay.addEventListener('click', closePanel);
        panel.addEventListener('click', function (e) { e.stopPropagation(); });

        return { open: openPanel, close: closePanel, refreshList: renderList };
    }

    window.__rpcDesktopProcessManager = {
        STORAGE_KEY: STORAGE_KEY,
        loadEntries: loadEntries,
        saveEntries: saveEntries,
        resolveExePath: resolveExePath,
        isProbablyRunnablePath: isProbablyRunnablePath,
        normalizeEntry: normalizeEntry,
        mountPanel: mountPanel,
        generateEntryId: generateEntryId,
    };
})();
