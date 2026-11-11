<?php

namespace OPNsense\LunaShare\Api;

use OPNsense\Base\ApiControllerBase;
use OPNsense\Core\Backend;

/**
 * People who joined the server with someone's invite code: the server remembers them and keeps them limited to
 * the people who invited them. Releasing one makes it an ordinary device (its whole account).
 */
class InvitedController extends ApiControllerBase
{
    public function listAction()
    {
        $out = trim((new Backend())->configdRun('lunashare invited'));
        $rows = json_decode($out, true);
        return ['rows' => is_array($rows) ? $rows : [], 'error' => is_array($rows) ? '' : $out];
    }

    private function run($action, $key)
    {
        if ($this->request->isPost() && preg_match('/^[0-9a-f]{8,64}$/', (string)$key)) {
            $out = (new Backend())->configdpRun('lunashare ' . $action, [$key]);
            return ['status' => 'ok', 'output' => trim($out)];
        }
        return ['status' => 'failed'];
    }

    public function releaseAction($key = '')
    {
        return $this->run('release', $key);
    }

    public function restrictAction($key = '')
    {
        return $this->run('restrict', $key);
    }

    /** everything the server keeps about that person (record, offline messages, usernames, invite codes they made) */
    public function eraseAction($key = '')
    {
        return $this->run('erase', $key);
    }

    /** erase, and keep all their devices out */
    public function blockAction($key = '')
    {
        return $this->run('block', $key);
    }

    public function unblockAction($key = '')
    {
        return $this->run('unblock', $key);
    }
}
