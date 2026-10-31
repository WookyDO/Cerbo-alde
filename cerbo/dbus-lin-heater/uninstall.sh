#!/bin/sh
# Stoppt dbus-lin-heater und entfernt den Autostart. /data/dbus-lin-heater bleibt erhalten.
DIR=/data/dbus-lin-heater
if [ -L /service/dbus-lin-heater ]; then
    svc -d /service/dbus-lin-heater /service/dbus-lin-heater/log
    rm /service/dbus-lin-heater
fi
[ -f /data/rc.local ] && sed -i "\|$DIR/install.sh|d" /data/rc.local
pkill -f "$DIR/dbus_lin_heater.py" || true
echo "dbus-lin-heater entfernt."
