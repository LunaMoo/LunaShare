<?php

namespace OPNsense\LunaShare\Api;

use OPNsense\Base\ApiMutableServiceControllerBase;
use OPNsense\Core\Backend;

class ServiceController extends ApiMutableServiceControllerBase
{
    protected static $internalServiceClass = '\OPNsense\LunaShare\General';
    protected static $internalServiceTemplate = 'OPNsense/LunaShare';
    protected static $internalServiceEnabled = 'enabled';
    protected static $internalServiceName = 'lunashare';

    /**
     * Apply settings and reload the firewall so the optional WAN rule follows the settings.
     */
    public function reconfigureAction()
    {
        $result = parent::reconfigureAction();
        (new Backend())->configdRun('filter reload');
        return $result;
    }
}
