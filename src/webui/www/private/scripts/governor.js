// SPDX-License-Identifier: GPL-2.0-or-later
'use strict';

window.qBittorrent = window.qBittorrent || {};
window.qBittorrent.GovernorEditor = (function() {
    const MiB = 1024 * 1024;
    const GiB = 1024 * MiB;
    const fields = [
        ['PeriodSeconds', 60, '统计周期'],
        ['RecoveryDelaySeconds', 1, '恢复延迟'],
        ['AverageUploadBps', MiB, '触发均速'],
        ['ReleaseAverageBps', MiB, '解除均速'],
        ['BrakeBufferBytes', GiB, '刹车缓冲'],
        ['SoftUploadBps', MiB, '后段上传限速'],
        ['BrakeUploadBps', 1024, '上传刹车速度'],
        ['BrakeDownloadBps', 1024, '下载刹车速度'],
        ['UploadProgress', 0.01, '上传介入进度'],
        ['DownloadProgress', 0.01, '下载介入进度'],
        ['LookaheadSeconds', 1, '预测时间']
    ];

    function validate(values) {
        const integerRange = (key, min, max) => Number.isSafeInteger(values[key])
            && values[key] >= min && values[key] <= max;
        if (!integerRange('PeriodSeconds', 10, 2592000)) return '统计周期须为 10 秒至 30 天。';
        if (!integerRange('RecoveryDelaySeconds', 0, values.PeriodSeconds - 1)) return '恢复延迟须小于统计周期。';
        for (const key of ['AverageUploadBps', 'ReleaseAverageBps', 'SoftUploadBps', 'BrakeUploadBps', 'BrakeDownloadBps']) {
            if (!integerRange(key, 1, 2147483647)) return '速度须为正数，换算后不能超过 2147483647 B/s。';
        }
        if (values.ReleaseAverageBps >= values.AverageUploadBps) return '解除均速须低于触发均速。';
        if (values.SoftUploadBps >= values.AverageUploadBps) return '后段上传限速须低于触发均速。';
        if (values.BrakeUploadBps > values.SoftUploadBps) return '上传刹车速度须不高于后段上传限速。';
        if (!integerRange('BrakeBufferBytes', 0, values.AverageUploadBps * values.PeriodSeconds - 1)) return '刹车缓冲须小于整个周期的上传预算。';
        if (!Number.isFinite(values.UploadProgress) || !Number.isFinite(values.DownloadProgress)
            || values.UploadProgress < 0 || values.DownloadProgress > 1
            || values.UploadProgress > values.DownloadProgress) return '进度须在 0–100% 内，下载介入进度须不低于上传介入进度。';
        if (!integerRange('LookaheadSeconds', 0, 300)) return '预测时间须为 0–300 秒。';
        for (const key of ['TargetTags', 'TargetCategories']) {
            const parts = values[key].split(',');
            if (values[key].length > 4096 || parts.length > 128
                || parts.some(part => part.trim().length > 255)
                || /[\u0000-\u001f\u007f-\u009f]/.test(values[key])) return '标签或分类过长、数量过多或含控制字符。';
        }
        return '';
    }

    function summary(values) {
        const budget = values.AverageUploadBps * values.PeriodSeconds;
        return '周期上传预算 ' + (budget / GiB).toFixed(3) + ' GiB；硬刹车阈值 '
            + ((budget - values.BrakeBufferBytes) / GiB).toFixed(3) + ' GiB。';
    }

    function mount(root) {
        if (!root || root.dataset.mounted) return;
        root.dataset.mounted = 'true';
        const find = key => root.querySelector('[data-governor="' + key + '"]');
        const status = root.querySelector('[data-role="status"]');
        const preview = root.querySelector('[data-role="preview"]');
        const error = root.querySelector('[data-role="error"]');
        const save = root.querySelector('[data-action="save"]');
        const reload = root.querySelector('[data-action="reload"]');
        const defaults = root.querySelector('[data-action="defaults"]');
        const inputs = Array.from(root.querySelectorAll('[data-governor]'));
        const current = root.querySelector('[data-role="active"]');
        let state = null;
        let busy = false;
        let dirty = false;

        function read() {
            const values = {Enabled: find('Enabled').checked,
                TargetTags: find('TargetTags').value.trim(),
                TargetCategories: find('TargetCategories').value.trim()};
            for (const [key, unit, label] of fields) {
                const text = find(key).value.trim();
                if (!text || !Number.isFinite(Number(text))) throw new Error(label + '须为有效数字。');
                const scaled = Number(text) * unit;
                values[key] = key.endsWith('Progress') ? scaled : Math.round(scaled);
            }
            const problem = validate(values);
            if (problem) throw new Error(problem);
            return values;
        }

        function populate(values) {
            find('Enabled').checked = values.Enabled;
            find('TargetTags').value = values.TargetTags;
            find('TargetCategories').value = values.TargetCategories;
            for (const [key, unit] of fields) find(key).value = values[key] / unit;
            update();
        }

        function update() {
            try {
                const values = read();
                preview.textContent = summary(values) + (values.Enabled
                    && !values.TargetTags && !values.TargetCategories ? ' 当前未指定管理对象。' : '');
                error.textContent = '';
                save.disabled = busy || !state;
            }
            catch (problem) {
                error.textContent = problem.message;
                preview.textContent = '';
                save.disabled = true;
            }
        }

        function lock(value) {
            busy = value;
            inputs.forEach(input => { input.disabled = busy || !state; });
            reload.disabled = busy;
            defaults.disabled = busy || !state;
            update();
        }

        function display(response) {
            if (!response || typeof response.revision !== 'string' || !response.saved || !response.active || !response.defaults)
                throw new Error('服务器返回了不完整的策略配置。');
            state = response;
            dirty = false;
            populate(response.saved);
            current.replaceChildren();
            const rows = [
                ['自动限速', response.active.Enabled ? '已启用' : '未启用'],
                ['管理标签 / 分类', (response.active.TargetTags || '无') + ' / ' + (response.active.TargetCategories || '无')],
                ...fields.map(([key, unit, label]) => [label, String(response.active[key] / unit)
                    + (key.endsWith('Progress') ? '%' : (key.endsWith('Bps') ? (unit === MiB ? ' MiB/s' : ' KiB/s')
                        : (key === 'BrakeBufferBytes' ? ' GiB' : (key === 'PeriodSeconds' ? ' 分钟' : ' 秒'))))])
            ];
            for (const [label, value] of rows) {
                const row = document.createElement('tr');
                const name = document.createElement('th');
                const cell = document.createElement('td');
                name.scope = 'row';
                name.textContent = label;
                cell.textContent = value;
                row.append(name, cell);
                current.append(row);
            }
            status.textContent = !response.saved_valid ? '已保存的配置存在错误，请修正后重新保存。'
                : (response.pending_restart ? '已保存，等待重启 qBittorrent 后生效；当前运行策略见下方。'
                    : '已保存的配置与当前运行策略一致。');
        }

        async function request(path, options) {
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 15000);
            try {
                const response = await fetch('api/v2/app/' + path, Object.assign({
                    credentials: 'same-origin', cache: 'no-store', signal: controller.signal
                }, options));
                if (!response.ok) {
                    const text = await response.text();
                    throw new Error(response.status === 409 ? '配置已被其他页面修改，请点击“重新读取”后再保存。'
                        : ('请求失败 (' + response.status + ')：' + text.slice(0, 500)));
                }
                return await response.json();
            }
            finally { clearTimeout(timeout); }
        }

        async function load() {
            lock(true);
            status.textContent = '正在读取限速策略…';
            try {
                const response = await request('governorPreferences');
                if (root.isConnected) display(response);
            }
            catch (problem) {
                if (root.isConnected) status.textContent = '读取失败：' + problem.message;
            }
            finally { if (root.isConnected) lock(false); }
        }

        save.addEventListener('click', async function() {
            if (busy || !state) return;
            let values;
            try { values = read(); }
            catch (problem) { error.textContent = problem.message; return; }
            const body = new URLSearchParams({json: JSON.stringify(values), revision: state.revision});
            lock(true);
            status.textContent = '正在保存限速策略…';
            try {
                const response = await request('setGovernorPreferences', {method: 'POST', body: body});
                if (root.isConnected) display(response);
            }
            catch (problem) {
                if (root.isConnected) status.textContent = '未确认保存成功：' + problem.message;
            }
            finally { if (root.isConnected) lock(false); }
        });
        reload.addEventListener('click', function() {
            if (!dirty || window.confirm('重新读取会丢弃此区域未保存的修改，是否继续？')) load();
        });
        defaults.addEventListener('click', function() {
            if (state && !busy) {
                populate(state.defaults);
                dirty = true;
                status.textContent = '默认值已填入，尚未保存。';
            }
        });
        inputs.forEach(input => input.addEventListener('input', function() {
            if (state && !busy) {
                dirty = true;
                status.textContent = '表单已修改，尚未保存；保存后需重启生效。';
            }
            update();
        }));
        root.governorMayClose = function() {
            if (busy) {
                window.alert('限速策略正在读取或保存，请等待完成。');
                return false;
            }
            return !dirty || window.confirm('限速策略尚未单独保存。继续将只保存其他设置并关闭窗口，是否继续？');
        };
        load();
    }

    return {mount: mount, validate: validate};
})();
