<script>
    $( document ).ready(function() {
        mapDataToFormUI({'frm_general_settings': "/api/lunashare/settings/get"}).done(function () {
            formatTokenizersUI();
            $('.selectpicker').selectpicker('refresh');
        });
        updateServiceControlUI('lunashare');

        // people who joined with an invite code
        function esc(s) { return $('<div/>').text(s === undefined || s === null ? '' : String(s)).html(); }
        function day(ms) { return ms ? new Date(ms).toLocaleString() : '-'; }
        function loadInvited(done) {
            ajaxGet('/api/lunashare/invited/list', {}, function (data) {
                const body = $('#invited_rows').empty();
                const rows = (data && data.rows) ? data.rows : [];
                if (typeof done === 'function') done(rows);
                if (!rows.length) {
                    body.append('<tr><td colspan="7">{{ lang._('No one has joined with an invite code yet.') }}</td></tr>');
                    return;
                }
                rows.sort(function (a, b) { return (b.last || 0) - (a.last || 0); });
                rows.forEach(function (r) {
                    const key = String(r.key || '');
                    const b = function (act, cls, label, ask) {
                        return '<button class="btn btn-xs ' + cls + ' inv-act" style="margin:1px" data-act="' + act + '" data-key="' + esc(key) +
                            '" data-name="' + esc(r.name) + '"' + (ask ? ' data-ask="1"' : '') + '>' + label + '</button>';
                    };
                    let btn = '';
                    if (r.blocked) {
                        btn = b('unblock', 'btn-default', '{{ lang._('Unblock') }}');
                    } else {
                        btn = r.released ? b('restrict', 'btn-default', '{{ lang._('Limit again') }}') : b('release', 'btn-primary', '{{ lang._('Release') }}');
                        btn += ' ' + b('block', 'btn-warning', '{{ lang._('Block') }}', true);
                    }
                    btn += ' ' + b('erase', 'btn-danger', '{{ lang._('Delete') }}', true);
                    body.append('<tr><td>' + esc(r.name) + '<br/><small><code>' + esc(key.substr(0, 16)) + '</code></small></td><td>' +
                        esc(r.by || '?') + (r.byKey ? '<br/><small><code>' + esc(String(r.byKey).substr(0, 16)) + '</code></small>' : '') +
                        '</td><td>' + (r.room ? esc(r.room) : '<small>{{ lang._('none (only the inviter)') }}</small>') + '</td><td>' + (r.blocked ? '<b class="text-danger">{{ lang._('blocked') }}</b>' : r.released ? '{{ lang._('ordinary') }}' : '<b>{{ lang._('limited') }}</b>') +
                        '</td><td>' + esc(day(r.first)) + '</td><td>' + esc(day(r.last)) + '</td><td>' + btn + '</td></tr>');
                });
            });
        }
        $(document).on('click', '.inv-act', function () {
            const b = $(this);
            if (b.data('ask')) {
                const msg = b.data('act') === 'block'
                    ? '{{ lang._('Block %s? Everything the server keeps about them is deleted (offline messages, usernames, invite codes they made), their devices are signed out and none of them (same account or same phone / computer) can use this server again.') }}'
                    : '{{ lang._('Delete everything the server keeps about %s? Their record, offline messages, usernames and the invite codes they made are deleted and their devices are signed out. If they use an invite again, they come back as new.') }}';
                if (!window.confirm(msg.replace('%s', b.data('name') || b.data('key')))) return;
            }
            b.prop('disabled', true);
            const act = b.data('act'), key = String(b.data('key'));
            $('#invited_msg').hide();
            ajaxCall('/api/lunashare/invited/' + act + '/' + key, {}, function (res) {
                if (!res || res.status !== 'ok') {
                    $('#invited_msg').text('{{ lang._('The command failed.') }} ' + (res && res.output ? res.output : '')).show();
                    loadInvited();
                    return;
                }
                // the server applies it within a few seconds: check that it did
                setTimeout(function () {
                    loadInvited(function (rows) {
                        const r = rows.find(function (x) { return String(x.key) === key; });
                        const ok = act === 'erase' ? !r : act === 'block' ? r && r.blocked : act === 'unblock' ? r && !r.blocked
                                 : act === 'release' ? r && r.released : act === 'restrict' ? r && !r.released : true;
                        if (!ok) $('#invited_msg').text('{{ lang._('The server has not applied it. Is the service running, and was it restarted after the plugin was updated? Restart it (Services) and try again.') }}').show();
                    });
                }, 7000);
            });
        });
        $('#invited_refresh').click(loadInvited);
        loadInvited();

        $("#reconfigureAct").SimpleActionButton({
            onPreAction: function () {
                const dfObj = new $.Deferred();
                saveFormToEndpoint("/api/lunashare/settings/set", 'frm_general_settings', function () {
                    dfObj.resolve();
                }, true, function () {
                    dfObj.reject();
                });
                return dfObj;
            },
            onAction: function () {
                updateServiceControlUI('lunashare');
            }
        });
    });
</script>

<div class="content-box" style="padding-bottom: 1.5em;">
    {{ partial("layout_partials/base_form",['fields':generalForm,'id':'frm_general_settings'])}}
    <div class="col-md-12">
        <hr/>
        <button class="btn btn-primary" id="reconfigureAct"
                data-endpoint="/api/lunashare/service/reconfigure"
                data-label="{{ lang._('Apply') }}"
                data-error-title="{{ lang._('Error reconfiguring LunaShare rendezvous') }}"
                type="button"></button>
    </div>
</div>

<div class="content-box" style="padding: 1em; margin-top: 1em;">
    <h3>{{ lang._('People who joined with an invite code') }}</h3>
    <p>{{ lang._("The server remembers devices that joined with someone's private invite code. They only see and reach the people who invited them (and their own other devices), are never listed in rooms and can't use usernames - even if they later enter this server's address by hand. Release a device to treat it (and its account) like everyone else. Delete removes every trace of that person from this server; Block also keeps all their devices out.") }}</p>
    <table class="table table-striped table-condensed">
        <thead>
            <tr>
                <th>{{ lang._('Device') }}</th>
                <th>{{ lang._('Invited by') }}</th>
                <th>{{ lang._('Invite room') }}</th>
                <th>{{ lang._('Treatment') }}</th>
                <th>{{ lang._('First seen') }}</th>
                <th>{{ lang._('Last seen') }}</th>
                <th></th>
            </tr>
        </thead>
        <tbody id="invited_rows"></tbody>
    </table>
    <div class="alert alert-warning" id="invited_msg" style="display: none;"></div>
    <button class="btn btn-default btn-xs" id="invited_refresh" type="button">{{ lang._('Refresh') }}</button>
</div>
