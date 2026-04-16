/**
 * 信令地址（可直接用 file:// 打开页面；修改后保存刷新即可）
 *
 * 浏览器安全策略禁止在 file:// 下用 XHR/fetch 读 signaling_config.json，
 * 故可编辑配置放在本脚本中，由 script 标签加载。
 *
 * 优先级仍由 rpc_config.js 统一处理：URL ?signaling= > 此处项。
 */
(function (global) {
    'use strict';
    global.__rpcFrontendConfig = {
        signalingBaseUrl: '',
        signalingHost: '192.168.3.15',
        signalingPort: 9090,
    };
})(typeof window !== 'undefined' ? window : this);
