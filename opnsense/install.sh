#!/bin/sh
# Installs the LunaShare rendezvous plugin on an OPNsense box (run as root over SSH, from the unpacked archive).
#   sh install.sh            install / update
#   sh install.sh uninstall  remove everything again
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
FILES="opnsense/mvc/app/models/OPNsense/LunaShare opnsense/mvc/app/controllers/OPNsense/LunaShare
opnsense/mvc/app/views/OPNsense/LunaShare opnsense/service/templates/OPNsense/LunaShare
opnsense/service/conf/actions.d/actions_lunashare.conf etc/inc/plugins.inc.d/lunashare.inc etc/rc.d/lunashare_rendezvous opnsense/scripts/OPNsense/LunaShare"

refresh() {
    rm -f /tmp/opnsense_menu_cache.xml /tmp/opnsense_acl_cache.json /var/lib/php/tmp/opnsense_menu_cache.xml 2>/dev/null || true
    service configd restart
    configctl template reload OPNsense/LunaShare >/dev/null 2>&1 || true
    configctl filter reload >/dev/null 2>&1 || true
}

if [ "$1" = "uninstall" ]; then
    /usr/local/etc/rc.d/lunashare_rendezvous onestop 2>/dev/null || true
    for f in $FILES; do rm -rf "/usr/local/$f"; done
    rm -f /usr/local/bin/lunashare-rendezvous /etc/rc.conf.d/lunashare_rendezvous
    rm -rf /usr/local/etc/lunashare
    refresh
    echo "LunaShare rendezvous removed. (Its settings stay in config.xml until you remove <lunashare> there.)"
    exit 0
fi

[ -f "$HERE/bin/lunashare-rendezvous" ] || { echo "bin/lunashare-rendezvous missing"; exit 1; }
install -m 0755 "$HERE/bin/lunashare-rendezvous" /usr/local/bin/lunashare-rendezvous
mkdir -p /usr/local/etc/lunashare
(cd "$HERE/src" && tar cf - .) | (cd /usr/local && tar xpf -)
chmod 0755 /usr/local/etc/rc.d/lunashare_rendezvous /usr/local/opnsense/scripts/OPNsense/LunaShare/*.php
refresh
# a running server keeps the old program (and ignores commands it doesn't know): start the new one
if /usr/local/etc/rc.d/lunashare_rendezvous status >/dev/null 2>&1; then
    /usr/local/etc/rc.d/lunashare_rendezvous restart && echo "Restarted the running server with the new version."
fi
echo "Installed. Open Services > LunaShare rendezvous in the web UI, enable it and press Apply."
