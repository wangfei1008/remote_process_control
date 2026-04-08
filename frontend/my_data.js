(function () {
    'use strict';
    var ENABLE_UPLOAD_VERBOSE_LOG = false;

    var ID_CHARS_LOWER = '0123456789abcdefghijklmnopqrstuvwxyz';

    function formatSize(size) {
        if (!size) return '0 B';
        var units = ['B', 'KB', 'MB', 'GB'];
        var v = Number(size);
        var i = 0;
        while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
        return v.toFixed(i === 0 ? 0 : 2) + ' ' + units[i];
    }

    function formatTime(ms) {
        if (!ms) return '-';
        try { return new Date(ms).toLocaleString(); } catch (_) { return '-'; }
    }

    function normalizePath(path) {
        if (!path) return '';
        var p = String(path).replace(/\\/g, '/');
        p = p.replace(/^\/+/, '').replace(/\/+$/, '');
        return p;
    }

    function bytesToBase64(uint8) {
        var CHUNK = 0x8000;
        var index = 0;
        var str = '';
        while (index < uint8.length) {
            var slice = uint8.subarray(index, Math.min(index + CHUNK, uint8.length));
            str += String.fromCharCode.apply(null, slice);
            index += CHUNK;
        }
        return btoa(str);
    }

    function base64ToBytes(base64) {
        var bin = atob(base64 || '');
        var out = new Uint8Array(bin.length);
        for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
        return out;
    }

    var state = {
        clientId: 'file_' + window.__rpcRandomId(8, ID_CHARS_LOWER),
        ws: null,
        pc: null,
        dc: null,
        currentPath: '',
        selected: null,
        entries: [],
        waiters: [],
        logGroups: {},
        logGroupOrder: [],
        logFilter: 'all',
        collapseAll: false,
    };

    var dom = {
        tbody: document.getElementById('tbody'),
        currentFolderName: document.getElementById('current-folder-name'),
        preview: document.getElementById('preview'),
        logList: document.getElementById('log-list'),
        logFilter: document.getElementById('log-filter'),
        logClear: document.getElementById('log-clear'),
        logToggleCollapse: document.getElementById('log-toggle-collapse'),
        uploadProgress: document.getElementById('upload-progress'),
        btnUpload: document.getElementById('btn-upload'),
        btnRefresh: document.getElementById('btn-refresh'),
        btnDownload: document.getElementById('btn-download'),
        uploadInput: document.getElementById('upload-input'),
    };

    function htmlEscape(s) {
        return String(s || '').replace(/[&<>"']/g, function (c) {
            return ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c];
        });
    }

    function buildGroupTitle(key, category) {
        if (category === 'upload' && key) {
            var p1 = key.split(':');
            return '上传任务 · ' + (p1[1] || key);
        }
        if (category === 'download' && key) {
            var p2 = key.split(':');
            return '下载任务 · ' + (p2[1] || key);
        }
        if (category === 'error') return '错误日志';
        return '系统日志';
    }

    function touchGroupOrder(groupId) {
        state.logGroupOrder = state.logGroupOrder.filter(function (id) { return id !== groupId; });
        state.logGroupOrder.unshift(groupId);
    }

    function shouldShowGroup(group) {
        if (!group) return false;
        if (state.logFilter === 'all') return true;
        if (group.category === state.logFilter) return true;
        for (var i = 0; i < group.items.length; i++) {
            if (group.items[i].category === state.logFilter) return true;
        }
        return false;
    }

    function renderLogs() {
        if (!dom.logList) return;
        dom.logList.innerHTML = '';
        var visible = 0;
        state.logGroupOrder.forEach(function (groupId) {
            var group = state.logGroups[groupId];
            if (!shouldShowGroup(group)) return;
            visible += 1;

            var latest = group.items[0];
            var wrap = document.createElement('div');
            wrap.className = 'log-group';

            var header = document.createElement('div');
            header.className = 'log-group-header';
            header.innerHTML =
                '<div class="log-group-title">' + htmlEscape(group.title) + '</div>' +
                '<div class="log-group-latest">[' + htmlEscape(latest ? latest.time : '-') + '] ' +
                htmlEscape(latest ? latest.msg : '') + '</div>';

            var body = document.createElement('div');
            body.className = 'log-group-body';
            body.style.display = group.collapsed ? 'none' : 'block';
            group.items.forEach(function (entry) {
                var item = document.createElement('div');
                item.className = 'log-item';
                item.innerHTML = '<span class="log-time">[' + htmlEscape(entry.time) + ']</span>' + htmlEscape(entry.msg);
                body.appendChild(item);
            });

            header.addEventListener('click', function () {
                group.collapsed = !group.collapsed;
                renderLogs();
            });

            wrap.appendChild(header);
            wrap.appendChild(body);
            dom.logList.appendChild(wrap);
        });
        if (!visible) {
            var empty = document.createElement('div');
            empty.className = 'log-empty';
            empty.textContent = '暂无匹配日志';
            dom.logList.appendChild(empty);
        }
    }

    function log(msg, key, category) {
        category = category || 'system';
        var now = new Date().toLocaleTimeString();
        try { console.info('[my_data][' + category + '] ' + msg); } catch (_) {}
        var groupId = key || (category === 'error' ? 'errors' : 'system');
        if (!state.logGroups[groupId]) {
            state.logGroups[groupId] = {
                id: groupId,
                title: buildGroupTitle(key, category),
                category: category,
                collapsed: !!state.collapseAll,
                items: [],
            };
        }
        var group = state.logGroups[groupId];
        group.title = buildGroupTitle(key, category);
        group.category = category;

        if (key) {
            if (group.items.length > 0) {
                group.items[0].msg = msg;
                group.items[0].time = now;
                group.items[0].category = category;
            } else {
                group.items.unshift({ msg: msg, time: now, category: category });
            }
        } else {
            group.items.unshift({ msg: msg, time: now, category: category });
            if (group.items.length > 80) group.items.length = 80;
        }

        touchGroupOrder(groupId);
        renderLogs();
    }

    function verboseUploadLog(msg, key, category) {
        if (!ENABLE_UPLOAD_VERBOSE_LOG) return;
        log(msg, key, category || 'system');
    }

    function sendDc(payload) {
        if (!state.dc || state.dc.readyState !== 'open') throw new Error('DataChannel 未连接');
        state.dc.send(JSON.stringify(payload));
    }

    function sendDcAsync(payload) {
        try {
            sendDc(payload);
            return Promise.resolve();
        } catch (err) {
            return Promise.reject(err);
        }
    }

    function setUploadProgress(name, uploaded, total) {
        if (!dom.uploadProgress) return;
        if (!total || total <= 0) {
            dom.uploadProgress.textContent = '上传进度: -';
            return;
        }
        var percent = Math.min(100, Math.floor((uploaded * 100) / total));
        dom.uploadProgress.textContent = '上传进度: ' + name + ' ' + percent + '% (' + formatSize(uploaded) + '/' + formatSize(total) + ')';
    }

    function waitMessage(predicate, timeoutMs) {
        return new Promise(function (resolve, reject) {
            var done = false;
            var waiter = {
                match: predicate,
                resolve: function (msg) {
                    if (done) return;
                    done = true;
                    clearTimeout(timer);
                    resolve(msg);
                },
            };
            var timer = setTimeout(function () {
                if (done) return;
                done = true;
                state.waiters = state.waiters.filter(function (w) { return w !== waiter; });
                reject(new Error('等待消息超时'));
            }, timeoutMs || 15000);
            state.waiters.push(waiter);
        });
    }

    function waitUploadMessage(transferId, successType, timeoutMs) {
        return waitMessage(function (m) {
            if (!m || typeof m.type !== 'string') return false;
            if (m.type === successType) {
                return !transferId || m.transferId === transferId;
            }
            if (m.type === 'fileError') {
                return !transferId || m.transferId === transferId;
            }
            return false;
        }, timeoutMs || 15000).then(function (m) {
            if (m.type === 'fileError') {
                throw new Error(m.message || ('上传失败(' + (m.op || 'unknown') + ')'));
            }
            return m;
        });
    }

    function dispatchMessage(msg) {
        var consumed = false;
        state.waiters.slice().forEach(function (w) {
            if (w.match(msg)) {
                consumed = true;
                state.waiters = state.waiters.filter(function (x) { return x !== w; });
                w.resolve(msg);
            }
        });
        return consumed;
    }

    function renderList() {
        state.currentPath = normalizePath(state.currentPath);
        var currentFolderLabel = state.currentPath ? ('/' + state.currentPath) : '/';
        if (dom.currentFolderName) dom.currentFolderName.textContent = currentFolderLabel;
        dom.tbody.innerHTML = '';

        var sorted = state.entries.slice().sort(function (a, b) {
            if (!!a.isDir !== !!b.isDir) return a.isDir ? -1 : 1;
            return String(a.name).localeCompare(String(b.name));
        });

        var parentPath = '';
        if (state.currentPath) {
            var idx = state.currentPath.lastIndexOf('/');
            parentPath = idx >= 0 ? state.currentPath.slice(0, idx) : '';
            var upEntry = {
                name: '..',
                isDir: true,
                isParent: true,
                path: parentPath,
                size: 0,
                mtime: 0,
            };
            sorted.unshift(upEntry);
        }

        sorted.forEach(function (e) {
            var tr = document.createElement('tr');
            tr.innerHTML = '<td>' + e.name + '</td><td>' + (e.isDir ? '文件夹' : '文件') + '</td><td>' +
                (e.isParent || e.isDir ? '-' : formatSize(e.size)) + '</td><td>' + (e.isParent ? '-' : formatTime(e.mtime)) + '</td>';
            tr.addEventListener('click', function () {
                state.selected = e;
                dom.btnDownload.disabled = !!e.isDir;
                if (!e.isDir) {
                    requestPreview(e.path).catch(function (err) {
                        dom.preview.textContent = '预览失败: ' + err.message;
                    });
                } else {
                    dom.preview.textContent = '请选择文本文件进行预览';
                }
            });
            tr.addEventListener('dblclick', function () {
                if (!e.isDir) return;
                state.currentPath = normalizePath(e.path || '');
                listCurrent();
            });
            dom.tbody.appendChild(tr);
        });
    }

    function listCurrent() {
        state.currentPath = normalizePath(state.currentPath);
        sendDc({ type: 'fileList', path: state.currentPath });
        return waitMessage(function (m) { return m.type === 'fileListResult'; }, 10000).then(function (res) {
            state.entries = (Array.isArray(res.entries) ? res.entries : []).map(function (entry) {
                var copy = Object.assign({}, entry);
                copy.path = normalizePath(copy.path || '');
                return copy;
            });
            if (res.root && state.currentPath === '') {
                log('已加载根目录: ' + res.root, 'system', 'system');
            }
            renderList();
        });
    }

    function requestPreview(path) {
        sendDc({ type: 'filePreview', path: path, maxBytes: 65536 });
        return waitMessage(function (m) {
            return m.type === 'filePreviewResult' && m.path === path;
        }, 10000).then(function (res) {
            if (!res.previewable) {
                dom.preview.textContent = '该类型暂不支持预览，请直接下载。';
                return;
            }
            dom.preview.textContent = String(res.content || '');
        });
    }

    function downloadSelected() {
        var item = state.selected;
        if (!item || item.isDir) return;
        var chunkSize = 8 * 1024;
        var chunks = [];
        var offset = 0;
        var downloadLogKey = 'download:' + item.path;
        log('开始下载: ' + item.path, downloadLogKey, 'download');

        sendDc({ type: 'fileDownloadInit', path: item.path, chunkSize: chunkSize });
        waitMessage(function (m) {
            return m.type === 'fileDownloadReady' && m.path === item.path;
        }, 10000).then(function (ready) {
            function pull() {
                sendDc({
                    type: 'fileDownloadChunk',
                    transferId: ready.transferId,
                    offset: offset,
                    size: ready.chunkSize || chunkSize,
                });
                return waitMessage(function (m) {
                    return m.type === 'fileDownloadChunkData' && m.transferId === ready.transferId && Number(m.offset) === offset;
                }, 15000).then(function (pack) {
                    if (pack.data) chunks.push(base64ToBytes(pack.data));
                    offset = Number(pack.nextOffset || offset);
                    log('下载进度: ' + formatSize(offset) + '/' + formatSize(ready.fileSize), downloadLogKey, 'download');
                    if (pack.isEof) {
                        var blob = new Blob(chunks, { type: 'application/octet-stream' });
                        var a = document.createElement('a');
                        a.href = URL.createObjectURL(blob);
                        a.download = ready.fileName || item.name || 'download.bin';
                        document.body.appendChild(a);
                        a.click();
                        a.remove();
                        setTimeout(function () { URL.revokeObjectURL(a.href); }, 1000);
                        log('下载完成: ' + (ready.fileName || item.name), downloadLogKey, 'download');
                        return;
                    }
                    return pull();
                });
            }
            return pull();
        }).catch(function (err) {
            log('下载失败: ' + err.message, downloadLogKey, 'error');
        });
    }

    function uploadFile(file) {
        var chunkSize = 4 * 1024;
        var uploadLogKey = 'upload:' + file.name + ':' + file.size + ':' + file.lastModified;
        var uploadDbgKey = uploadLogKey + ':dbg';
        var requestId = 'req_' + window.__rpcRandomId(12, ID_CHARS_LOWER);
        log('开始上传: ' + file.name, uploadLogKey, 'upload');
        verboseUploadLog('upload_init requestId=' + requestId + ' size=' + file.size + ' chunk=' + chunkSize + ' path=' + (state.currentPath || '/'), uploadDbgKey, 'system');
        setUploadProgress(file.name, 0, file.size);
        sendDc({
            type: 'fileUploadInit',
            requestId: requestId,
            path: state.currentPath,
            fileName: file.name,
            fileSize: file.size,
            chunkSize: chunkSize,
            overwrite: true,
            resume: false,
        });

        return waitMessage(function (m) {
            if (!m || typeof m.type !== 'string') return false;
            if (m.type === 'fileUploadReady') return m.requestId === requestId;
            if (m.type === 'fileError') return m.op === 'fileUploadInit';
            return false;
        }, 10000).then(function (ready) {
            if (ready.type === 'fileError') {
                throw new Error(ready.message || '上传初始化失败');
            }
            var offset = Number(ready.nextOffset || 0);
            var transferId = ready.transferId;
            var actualChunk = Number(ready.chunkSize || chunkSize);
            verboseUploadLog('upload_ready transferId=' + transferId + ' nextOffset=' + offset + ' chunk=' + actualChunk, uploadDbgKey, 'system');

            function pushChunk(retryCount) {
                retryCount = retryCount || 0;
                if (offset >= file.size) {
                    verboseUploadLog('upload_commit transferId=' + transferId + ' uploaded=' + offset + '/' + file.size, uploadDbgKey, 'system');
                    sendDc({ type: 'fileUploadCommit', transferId: transferId });
                    return waitUploadMessage(transferId, 'fileUploadCommitted', 20000).then(function () {
                        setUploadProgress(file.name, file.size, file.size);
                        log('上传完成: ' + file.name, uploadLogKey, 'upload');
                        verboseUploadLog('upload_committed transferId=' + transferId, uploadDbgKey, 'system');
                    });
                }
                var end = Math.min(file.size, offset + actualChunk);
                return file.slice(offset, end).arrayBuffer().then(function (ab) {
                    var bytes = new Uint8Array(ab);
                    verboseUploadLog('upload_chunk send transferId=' + transferId + ' offset=' + offset + ' bytes=' + bytes.length + ' retry=' + retryCount, uploadDbgKey, 'system');
                    return sendDcAsync({
                        type: 'fileUploadChunk',
                        transferId: transferId,
                        offset: offset,
                        data: bytesToBase64(bytes),
                    }).then(function () {
                        return waitUploadMessage(transferId, 'fileUploadAck', 60000).then(function (ack) {
                            if (!ack.ok) {
                                offset = Number(ack.expectedOffset || offset);
                                setUploadProgress(file.name, offset, file.size);
                                log('上传续传对齐到: ' + formatSize(offset), uploadLogKey, 'upload');
                                verboseUploadLog('upload_ack mismatch expected=' + offset, uploadDbgKey, 'error');
                                return pushChunk(0);
                            }
                            offset = Number(ack.uploadedBytes || offset);
                            verboseUploadLog('upload_ack ok uploaded=' + offset + '/' + file.size, uploadDbgKey, 'system');
                            setUploadProgress(file.name, offset, file.size);
                            log('上传进度: ' + formatSize(offset) + '/' + formatSize(file.size), uploadLogKey, 'upload');
                            return pushChunk(0);
                        }).catch(function (err) {
                            if (retryCount < 5) {
                                log('上传分片超时，重试(' + (retryCount + 1) + '/5): ' + formatSize(offset), uploadLogKey, 'error');
                                verboseUploadLog('upload_retry transferId=' + transferId + ' offset=' + offset + ' reason=' + err.message, uploadDbgKey, 'error');
                                return pushChunk(retryCount + 1);
                            }
                            throw err;
                        });
                    });
                });
            }
            return pushChunk(0);
        }).then(function () {
            return listCurrent();
        }).catch(function (err) {
            setUploadProgress(file.name, 0, file.size);
            log('上传失败: ' + file.name + ' - ' + err.message, uploadLogKey, 'error');
            verboseUploadLog('upload_failed requestId=' + requestId + ' err=' + err.message, uploadDbgKey, 'error');
        });
    }

    function createPeerAndAnswer(offerMessage) {
        var pc = new RTCPeerConnection({ bundlePolicy: 'max-bundle' });
        state.pc = pc;
        pc.ondatachannel = function (evt) {
            state.dc = evt.channel;
            state.dc.onopen = function () {
                log('DataChannel 打开');
                listCurrent().catch(function (err) { log('目录加载失败: ' + err.message); });
            };
            state.dc.onclose = function () { log('DataChannel 已关闭'); };
            state.dc.onmessage = function (ev) {
                if (typeof ev.data !== 'string') return;
                var msg = null;
                try { msg = JSON.parse(ev.data); } catch (_) { return; }
                if (!msg) return;
                if (msg.type === 'fileError') {
                    log('服务端错误[' + (msg.op || '-') + ']: ' + (msg.message || 'unknown'), '', 'error');
                }
                dispatchMessage(msg);
            };
        };

        return pc.setRemoteDescription(new RTCSessionDescription({
            type: offerMessage.type,
            sdp: offerMessage.sdp,
        })).then(function () {
            return pc.createAnswer();
        }).then(function (answer) {
            return pc.setLocalDescription(answer);
        }).then(function () {
            return new Promise(function (resolve) {
                if (pc.iceGatheringState === 'complete') resolve();
                else pc.addEventListener('icegatheringstatechange', function onGather() {
                    if (pc.iceGatheringState === 'complete') {
                        pc.removeEventListener('icegatheringstatechange', onGather);
                        resolve();
                    }
                });
            });
        }).then(function () {
            state.ws.send(JSON.stringify({
                id: 'server',
                type: state.pc.localDescription.type,
                sdp: state.pc.localDescription.sdp,
            }));
        });
    }

    function buildWsUrl(clientId) {
        if (typeof window.__rpcBuildSignalingWebSocketUrl === 'function') {
            return window.__rpcBuildSignalingWebSocketUrl(clientId);
        }
        var params = new URLSearchParams(window.location.search);
        var signaling = params.get('signaling');
        if (signaling) {
            var base = signaling.replace(/\/+$/, '');
            return base + '/' + clientId;
        }
        var proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        var host = window.location.hostname || '127.0.0.1';
        return proto + '//' + host + ':9090/' + clientId;
    }

    function connect() {
        log('日志系统已启动，正在连接信令服务...', 'system', 'system');
        var ws = new WebSocket(buildWsUrl(state.clientId));
        state.ws = ws;

        ws.onopen = function () {
            log('WebSocket 已连接');
            ws.send(JSON.stringify({ id: 'server', type: 'file_request' }));
        };
        ws.onclose = function () { log('信令已断开'); };
        ws.onerror = function () { log('信令错误'); };
        ws.onmessage = function (evt) {
            var msg = null;
            try { msg = JSON.parse(evt.data); } catch (_) { return; }
            if (!msg) return;
            if (msg.type === 'offer') {
                createPeerAndAnswer(msg).catch(function (err) {
                    log('建立会话失败: ' + err.message);
                });
            }
        };
    }

    dom.btnRefresh.addEventListener('click', function () {
        listCurrent().catch(function (err) { log('刷新失败: ' + err.message); });
    });
    dom.btnDownload.addEventListener('click', downloadSelected);
    if (dom.btnUpload) {
        dom.btnUpload.addEventListener('click', function () {
            if (!dom.uploadInput) return;
            dom.uploadInput.click();
        });
    }
    if (dom.logFilter) {
        dom.logFilter.addEventListener('change', function () {
            state.logFilter = dom.logFilter.value || 'all';
            renderLogs();
        });
    }
    if (dom.logClear) {
        dom.logClear.addEventListener('click', function () {
            state.logGroups = {};
            state.logGroupOrder = [];
            renderLogs();
        });
    }
    if (dom.logToggleCollapse) {
        dom.logToggleCollapse.addEventListener('click', function () {
            state.collapseAll = !state.collapseAll;
            state.logGroupOrder.forEach(function (id) {
                if (state.logGroups[id]) state.logGroups[id].collapsed = state.collapseAll;
            });
            dom.logToggleCollapse.textContent = state.collapseAll ? '全部展开' : '全部收起';
            renderLogs();
        });
    }
    dom.uploadInput.addEventListener('change', function () {
        if (!dom.uploadInput.files || !dom.uploadInput.files.length) return;
        var files = Array.prototype.slice.call(dom.uploadInput.files);
        files.reduce(function (p, file) {
            return p.then(function () { return uploadFile(file); });
        }, Promise.resolve()).finally(function () {
            dom.uploadInput.value = '';
            setUploadProgress('', 0, 0);
            listCurrent().catch(function () {});
        });
    });

    function startConnect() {
        connect();
    }
    if (typeof window.__rpcEnsureSignalingJsonLoaded === 'function') {
        window.__rpcEnsureSignalingJsonLoaded().then(startConnect).catch(startConnect);
    } else {
        startConnect();
    }
})();
