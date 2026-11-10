#!/usr/local/bin/php
<?php

/*
 * LunaShare: writes a certificate from the OPNsense trust store (with its CA chain) and its key where the
 * rendezvous server reads them, readable only by the server's user.
 *   export_cert.php <refid> <certificate file> <key file> <user>
 * SPDX-License-Identifier: GPL-3.0-only
 */

require_once('script/load_phalcon.php');

use OPNsense\Core\Config;

if ($argc < 5) {
    fwrite(STDERR, "usage: export_cert.php refid cert key user\n");
    exit(2);
}
list(, $refid, $certFile, $keyFile, $user) = $argv;
$cfg = Config::getInstance()->object();

function lunashare_find($list, $refid)
{
    if (!isset($list)) {
        return null;
    }
    foreach ($list as $item) {
        if ((string)$item->refid === $refid) {
            return $item;
        }
    }
    return null;
}

$cert = lunashare_find($cfg->cert, $refid);
if ($cert === null || empty((string)$cert->prv)) {
    fwrite(STDERR, "certificate $refid not found (or it has no private key)\n");
    exit(1);
}
$pem = rtrim(base64_decode((string)$cert->crt)) . "\n";
// the chain up to the root (browsers need the intermediates)
$caref = (string)$cert->caref;
for ($i = 0; $caref !== '' && $i < 5; $i++) {
    $ca = lunashare_find($cfg->ca, $caref);
    if ($ca === null) {
        break;
    }
    $pem .= rtrim(base64_decode((string)$ca->crt)) . "\n";
    $caref = (string)$ca->caref;
}
@mkdir(dirname($certFile), 0755, true);
$old = umask(0077);
$ok = file_put_contents($keyFile . '.new', base64_decode((string)$cert->prv)) !== false &&
      file_put_contents($certFile . '.new', $pem) !== false;
umask($old);
if (!$ok) {
    fwrite(STDERR, "cannot write the certificate files\n");
    exit(1);
}
foreach ([$keyFile, $certFile] as $f) {
    chown($f . '.new', $user);
    chmod($f . '.new', 0600);
    rename($f . '.new', $f);
}
exit(0);
