<?php

/*
 * LunaShare rendezvous - OPNsense plugin hooks (services list + optional WAN rule).
 * SPDX-License-Identifier: GPL-3.0-only
 */

function lunashare_settings()
{
    return new \OPNsense\LunaShare\General();
}

function lunashare_enabled()
{
    return (string)lunashare_settings()->enabled == '1';
}

function lunashare_services()
{
    $services = [];
    if (lunashare_enabled()) {
        $services[] = [
            'description' => gettext('LunaShare rendezvous'),
            'configd' => [
                'restart' => ['lunashare restart'],
                'start' => ['lunashare start'],
                'stop' => ['lunashare stop'],
            ],
            'name' => 'lunashare',
            'pidfile' => '/var/run/lunashare/lunashare_rendezvous.pid',
        ];
    }
    return $services;
}

function lunashare_firewall($fw)
{
    $mdl = lunashare_settings();
    if ((string)$mdl->enabled != '1' || (string)$mdl->autorule != '1') {
        return;
    }
    $fw->registerFilterRule(
        1,
        [
            'interface' => 'wan',
            'ipprotocol' => 'inet',
            'protocol' => 'udp',
            'to' => '(self)',
            'to_port' => (string)$mdl->port,
            'type' => 'pass',
            'direction' => 'in',
            'quick' => true,
            'statetype' => 'keep',
            'descr' => 'LunaShare rendezvous (automatic rule)',
            '#ref' => 'ui/lunashare/',
        ]
    );
    if ((string)$mdl->links == '1') {
        // links for anyone: devices keep a TCP connection on the rendezvous port; browsers open the pages
        $ports = [(string)$mdl->port];
        if ((string)$mdl->linkurl == '') {
            $ports[] = (string)$mdl->linkport;
        }
        foreach ($ports as $p) {
            $fw->registerFilterRule(
                1,
                [
                    'interface' => 'wan',
                    'ipprotocol' => 'inet',
                    'protocol' => 'tcp',
                    'to' => '(self)',
                    'to_port' => $p,
                    'type' => 'pass',
                    'direction' => 'in',
                    'quick' => true,
                    'statetype' => 'keep',
                    'descr' => 'LunaShare links for anyone (automatic rule)',
                    '#ref' => 'ui/lunashare/',
                ]
            );
        }
    }
}
