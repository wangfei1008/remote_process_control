/**
 * 随机 id（file:// / 非 module）
 * @param {number} length
 * @param {string} [charset] 默认：大小写字母 + 数字
 */
(function (global) {
    'use strict';
    var DEFAULT = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
    global.__rpcRandomId = function randomId(length, charset) {
        var n = Math.max(1, parseInt(length, 10) || 8);
        var chars = charset && String(charset).length ? String(charset) : DEFAULT;
        var out = '';
        var i;
        for (i = 0; i < n; i++) {
            out += chars.charAt(Math.floor(Math.random() * chars.length));
        }
        return out;
    };
})(typeof window !== 'undefined' ? window : this);
