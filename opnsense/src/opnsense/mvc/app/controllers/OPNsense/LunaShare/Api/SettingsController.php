<?php

namespace OPNsense\LunaShare\Api;

use OPNsense\Base\ApiMutableModelControllerBase;

class SettingsController extends ApiMutableModelControllerBase
{
    protected static $internalModelClass = '\OPNsense\LunaShare\General';
    protected static $internalModelName = 'general';
}
