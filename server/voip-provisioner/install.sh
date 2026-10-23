#!/bin/bash
# Install the Analog Redial VoIP provisioner on the Asterisk host.
# Run from this directory:
#
#   sudo ./install.sh --sip-server <host boards register with> \
#        [--context <dialplan context>] [--permit <cidr>]... [--port 8077] \
#        [--with-tts [--tts-voice af_sarah]]
#
# --with-tts also installs the text-to-speech service (redial_tts.py: a Python
# virtual environment with Kokoro, ~350 MB of model files), local to this
# machine, and points the provisioner at it so boards can make their spoken
# prompts from their own web page.
#
# Safe to re-run: it keeps an existing config and key, and never adds an
# include line twice. pjsip.conf and extensions.conf are backed up before
# the first change.
set -euo pipefail

SIP_SERVER=""
PORT=8077
CONTEXT=rotary
PERMIT=()
WITH_TTS=0
TTS_VOICE=af_sarah
while [ $# -gt 0 ]; do
    case "$1" in
        --sip-server)   SIP_SERVER="$2"; shift 2 ;;
        --port)         PORT="$2"; shift 2 ;;
        --context)      CONTEXT="$2"; shift 2 ;;
        --permit)       PERMIT+=("$2"); shift 2 ;;
        --with-tts)     WITH_TTS=1; shift ;;
        --tts-voice)    TTS_VOICE="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
[ -n "$SIP_SERVER" ] || { echo "--sip-server is required (the name or IP boards register with)" >&2; exit 1; }
id asterisk >/dev/null 2>&1 || { echo "no 'asterisk' user: is Asterisk installed?" >&2; exit 1; }
[ -f /etc/asterisk/pjsip.conf ] || { echo "/etc/asterisk/pjsip.conf not found" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
APP=/opt/redial-provisioner
ETC=/etc/redial-provisioner
CONFIG=$ETC/config.json
STATE=/var/lib/redial-provisioner
GEN=/etc/asterisk/redial
PY="python3 $APP/redial_provisioner.py"

echo "== files"
install -d -o root -g root -m 755 "$APP"
install -o root -g root -m 755 "$HERE/redial_provisioner.py" "$APP/"
install -d -o root -g asterisk -m 750 "$ETC"
install -d -o asterisk -g asterisk -m 700 "$STATE"
install -d -o asterisk -g asterisk -m 750 "$GEN"
for f in pjsip.conf extensions.conf; do
    [ -f "$GEN/$f" ] || install -o asterisk -g asterisk -m 640 /dev/null "$GEN/$f"
done

NEW_KEY=""
if [ -f "$CONFIG" ]; then
    echo "   keeping existing $CONFIG"
else
    SIP_SERVER="$SIP_SERVER" PORT="$PORT" PERMIT="${PERMIT[*]:-}" CONTEXT="$CONTEXT" python3 - "$CONFIG" <<'EOF'
import json, os, sys
cfg = {
    "name": "Analog Redial VoIP",
    "listen": "0.0.0.0",
    "port": int(os.environ["PORT"]),
    "sip_server": os.environ["SIP_SERVER"],
    "context": os.environ["CONTEXT"],
    "permit": os.environ["PERMIT"].split(),
    "keys": [],
}
with open(sys.argv[1], "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
EOF
    chown root:asterisk "$CONFIG"
    chmod 640 "$CONFIG"
    NEW_KEY=$($PY add-key --config "$CONFIG" home --label "first key")
fi

echo "== asterisk includes"
STAMP=$(date +%Y%m%d-%H%M%S)
add_line() {  # file line
    grep -qxF "$2" "$1" && return 0
    [ -f "$1.bak-redial-$STAMP" ] || cp -p "$1" "$1.bak-redial-$STAMP"
    printf '\n%s\n' "$2" >> "$1"
    echo "   $1: added '$2'"
}
add_line /etc/asterisk/pjsip.conf "#include redial/pjsip.conf"
touch /etc/asterisk/extensions.conf
add_line /etc/asterisk/extensions.conf "#include redial/extensions.conf"

if [ -d /etc/avahi/services ]; then
    echo "== mDNS advertisement"
    cat > /etc/avahi/services/redial-provisioner.service <<EOF
<?xml version="1.0" standalone='no'?>
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
  <name replace-wildcards="yes">Analog Redial VoIP on %h</name>
  <service>
    <type>_redial-prov._tcp</type>
    <port>$(python3 -c "import json;print(json.load(open('$CONFIG'))['port'])")</port>
    <txt-record>path=/api/v1</txt-record>
  </service>
</service-group>
EOF
fi

echo "== service"
install -o root -g root -m 644 "$HERE/redial-provisioner.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable redial-provisioner >/dev/null
systemctl restart redial-provisioner
sleep 2
systemctl --no-pager --lines=5 status redial-provisioner || true

if [ "$WITH_TTS" = 1 ]; then
    echo "== text-to-speech"
    TTS=/opt/redial-tts
    TTS_URL=https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0
    install -d -o root -g root -m 755 "$TTS" "$TTS/models"
    install -o root -g root -m 755 "$HERE/redial_tts.py" "$TTS/"
    install -d -o asterisk -g asterisk -m 750 /var/cache/redial-tts
    if [ ! -x "$TTS/venv/bin/python" ]; then
        python3 -m venv "$TTS/venv" || { echo "python3 -m venv failed: install python3-venv (apt install python3-venv)" >&2; exit 1; }
    fi
    "$TTS/venv/bin/pip" install -q --upgrade pip
    "$TTS/venv/bin/pip" install -q kokoro-onnx soundfile numpy
    for f in kokoro-v1.0.onnx voices-v1.0.bin; do
        if [ ! -s "$TTS/models/$f" ]; then
            echo "   downloading $f"
            curl -fL --retry 3 -o "$TTS/models/$f.part" "$TTS_URL/$f"
            mv "$TTS/models/$f.part" "$TTS/models/$f"
        fi
    done
    chmod 644 "$TTS/models/"*
    sed "s/@VOICE@/$TTS_VOICE/" "$HERE/redial-tts.service" > /etc/systemd/system/redial-tts.service
    chmod 644 /etc/systemd/system/redial-tts.service
    systemctl daemon-reload
    systemctl enable redial-tts >/dev/null
    systemctl restart redial-tts
    for i in $(seq 1 30); do
        curl -fs http://127.0.0.1:8078/health >/dev/null && break
        sleep 1
    done
    curl -fs http://127.0.0.1:8078/health || { echo "redial-tts didn't start: journalctl -u redial-tts" >&2; exit 1; }
    echo
    python3 - "$CONFIG" <<'EOF'
import json, sys
p = sys.argv[1]
cfg = json.load(open(p))
if cfg.get("tts_url") != "http://127.0.0.1:8078":
    cfg["tts_url"] = "http://127.0.0.1:8078"
    with open(p, "r+") as f:      # rewrite in place: keeps owner and mode
        f.seek(0); json.dump(cfg, f, indent=2); f.write("\n"); f.truncate()
    print("   provisioner now forwards text-to-speech to 127.0.0.1:8078")
EOF
fi

echo
echo "Provisioner running on port $(python3 -c "import json;print(json.load(open('$CONFIG'))['port'])")."
if [ -n "$NEW_KEY" ]; then
    echo "Enrollment key (enter it in each board's Settings -> VOIP -> Automatic setup):"
    echo
    echo "    $NEW_KEY"
    echo
    echo "It is stored in $CONFIG. Make more with:"
    echo "    sudo $PY add-key --config $CONFIG <name> --min 2000 --max 2099"
fi
echo "Board numbers are added to the [$(python3 -c "import json;print(json.load(open('$CONFIG'))['context'])")] dialplan context:"
echo "phones in that context can call them, and the boards' own calls land there."
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    echo "ufw is active: allow the port from your LAN only, e.g."
    echo "    sudo ufw allow from 192.168.0.0/24 to any port 8077 proto tcp"
fi
