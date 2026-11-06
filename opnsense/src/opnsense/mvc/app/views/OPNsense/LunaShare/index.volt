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
        function loadInvited() {
            ajaxGet('/api/lunashare/invited/list', {}, function (data) {
                const body = $('#invited_rows').empty();
                const rows = (data && data.rows) ? data.rows : [];
                if (!rows.length) {
                    body.append('<tr><td colspan="6">{{ lang._('No one has joined with an invite code yet.') }}</td></tr>');
                    return;
                }
                rows.sort(function (a, b) { return (b.last || 0) - (a.last || 0); });
                rows.forEach(function (r) {
                    const key = String(r.key || '');
                    const btn = r.released
                        ? '<button class="btn btn-xs btn-default inv-act" data-act="restrict" data-key="' + esc(key) + '">{{ lang._('Limit again') }}</button>'
                        : '<button class="btn btn-xs btn-primary inv-act" data-act="release" data-key="' + esc(key) + '">{{ lang._('Release') }}</button>';
                    body.append('<tr><td>' + esc(r.name) + '<br/><small><code>' + esc(key.substr(0, 16)) + '</code></small></td><td>' +
                        esc(r.by || '?') + '</td><td>' + (r.released ? '{{ lang._('ordinary') }}' : '<b>{{ lang._('limited') }}</b>') +
                        '</td><td>' + esc(day(r.first)) + '</td><td>' + esc(day(r.last)) + '</td><td>' + btn + '</td></tr>');
                });
            });
        }
        $(document).on('click', '.inv-act', function () {
            const b = $(this);
            b.prop('disabled', true);
            ajaxCall('/api/lunashare/invited/' + b.data('act') + '/' + b.data('key'), {}, function () {
                setTimeout(loadInvited, 6000);  // (the server applies it within a few seconds)
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
    <p>{{ lang._("The server remembers devices that joined with someone's private invite code. They only see and reach the people who invited them (and their own other devices), are never listed in rooms and can't use usernames - even if they later enter this server's address by hand. Release a device to treat it (and its account) like everyone else.") }}</p>
    <table class="table table-striped table-condensed">
        <thead>
            <tr>
                <th>{{ lang._('Device') }}</th>
                <th>{{ lang._('Invited by') }}</th>
                <th>{{ lang._('Treatment') }}</th>
                <th>{{ lang._('First seen') }}</th>
                <th>{{ lang._('Last seen') }}</th>
                <th></th>
            </tr>
        </thead>
        <tbody id="invited_rows"></tbody>
    </table>
    <button class="btn btn-default btn-xs" id="invited_refresh" type="button">{{ lang._('Refresh') }}</button>
</div>
