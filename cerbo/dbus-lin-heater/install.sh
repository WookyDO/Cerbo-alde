#!/bin/sh
# Aktiviert dbus-lin-heater unter Venus OS. Wird zusätzlich aus /data/rc.local bei
# jedem Start aufgerufen, weil /service nach Neustart/Firmware-Update leer ist.
set -e
DIR=/data/dbus-lin-heater
OLD=/data/dbus-alde

# Vorgänger dbus-alde ablösen, seine config.ini (host, Instanzen, Namen) übernehmen.
if [ -d "$OLD" ]; then
    [ -x "$OLD/uninstall.sh" ] && "$OLD/uninstall.sh" || true
    [ -f "$OLD/config.ini" ] && [ ! -f "$DIR/config.ini" ] && cp "$OLD/config.ini" "$DIR/config.ini"
    rm -rf "$OLD"
    echo "dbus-alde ersetzt."
fi

[ -f "$DIR/config.ini" ] || cp "$DIR/config.sample.ini" "$DIR/config.ini"
chmod +x "$DIR/dbus_lin_heater.py" "$DIR/install.sh" "$DIR/uninstall.sh" \
         "$DIR/service/run" "$DIR/service/log/run"

[ -L /service/dbus-lin-heater ] || ln -s "$DIR/service" /service/dbus-lin-heater

RC=/data/rc.local
if [ ! -f "$RC" ]; then
    printf '#!/bin/sh\n' > "$RC"
    chmod +x "$RC"
fi
grep -qF "$DIR/install.sh" "$RC" || echo "$DIR/install.sh" >> "$RC"

echo "dbus-lin-heater installiert. Logs: tail -F /var/log/dbus-lin-heater/current | tai64nlocal"
