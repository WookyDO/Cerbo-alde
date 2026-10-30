#!/bin/sh
# Stoppt dbus-alde und entfernt den Autostart. /data/dbus-alde bleibt erhalten.
DIR=/data/dbus-alde
if [ -L /service/dbus-alde ]; then
    svc -d /service/dbus-alde /service/dbus-alde/log
    rm /service/dbus-alde
fi
[ -f /data/rc.local ] && sed -i "\|$DIR/install.sh|d" /data/rc.local
pkill -f "$DIR/dbus_alde.py" || true
echo "dbus-alde entfernt."
