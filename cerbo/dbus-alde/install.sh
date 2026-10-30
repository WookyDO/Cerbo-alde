#!/bin/sh
# Aktiviert dbus-alde unter Venus OS. Wird zusätzlich aus /data/rc.local bei
# jedem Start aufgerufen, weil /service nach Neustart/Firmware-Update leer ist.
set -e
DIR=/data/dbus-alde

[ -f "$DIR/config.ini" ] || cp "$DIR/config.sample.ini" "$DIR/config.ini"
chmod +x "$DIR/dbus_alde.py" "$DIR/install.sh" "$DIR/uninstall.sh" \
         "$DIR/service/run" "$DIR/service/log/run"

[ -L /service/dbus-alde ] || ln -s "$DIR/service" /service/dbus-alde

RC=/data/rc.local
if [ ! -f "$RC" ]; then
    printf '#!/bin/sh\n' > "$RC"
    chmod +x "$RC"
fi
grep -qF "$DIR/install.sh" "$RC" || echo "$DIR/install.sh" >> "$RC"

echo "dbus-alde installiert. Logs: tail -F /var/log/dbus-alde/current | tai64nlocal"
