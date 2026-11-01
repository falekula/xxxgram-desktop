#!/usr/bin/env bash
set -euo pipefail
umask 077

payload=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
revision=$(cat "$payload/SOURCE_COMMIT")
[[ "$revision" =~ ^[0-9a-f]{40}$ ]] || { echo 'Некорректная версия сборки'; exit 1; }
[[ $(uname -m) == x86_64 ]] || { echo 'Нужен Linux x86_64'; exit 1; }
test -x "$payload/xxxgram"
test -f "$payload/proxy/requirements.txt"

install_root="$HOME/.local/opt/xxxgram"
version_dir="$install_root/$revision"
data_dir="$HOME/.local/share/xxxgram-desktop"
mkdir -p "$install_root" "$data_dir" "$HOME/.local/bin" "$HOME/.local/share/applications" "$HOME/.local/share/icons"
if [[ ! -d "$version_dir" ]]; then
    stage=$(mktemp -d "$install_root/.install-XXXXXXXX")
    trap 'rm -rf -- "$stage"' EXIT
    cp -a -- "$payload/." "$stage/"
    chmod 700 "$stage/xxxgram"
    if command -v uv >/dev/null 2>&1; then
        uv venv --python /usr/bin/python3 "$stage/proxy/venv"
        uv pip install --python "$stage/proxy/venv/bin/python3" --no-index \
            --find-links "$stage/proxy/wheels" -r "$stage/proxy/requirements.txt"
    else
        /usr/bin/python3 -m venv "$stage/proxy/venv"
        "$stage/proxy/venv/bin/python3" -m pip install --no-index \
            --find-links "$stage/proxy/wheels" -r "$stage/proxy/requirements.txt"
    fi
    "$stage/proxy/venv/bin/python3" -I -c 'import cryptography, httpx, h2, certifi'
    mv -- "$stage" "$version_dir"
    trap - EXIT
fi
test -x "$version_dir/xxxgram"
test -x "$version_dir/proxy/venv/bin/python3"
ln -s "$version_dir" "$install_root/.current-$$"
mv -Tf -- "$install_root/.current-$$" "$install_root/current"

cat > "$HOME/.local/bin/xxxgram" <<'LAUNCHER'
#!/usr/bin/env bash
set -euo pipefail
mkdir -p "$HOME/.local/share/xxxgram-desktop"
exec "$HOME/.local/opt/xxxgram/current/xxxgram" -workdir "$HOME/.local/share/xxxgram-desktop" "$@"
LAUNCHER
chmod 700 "$HOME/.local/bin/xxxgram"
icon="$HOME/.local/share/icons/org.xxxgram.desktop.png"
if [[ ! -f "$data_dir/xxxgram-icon.png" ]]; then
    install -m 600 "$version_dir/xxxgram.png" "$icon"
fi
export XXXGRAM_LAUNCHER="$HOME/.local/bin/xxxgram"
export XXXGRAM_ICON="$icon"
export XXXGRAM_DESKTOP="$HOME/.local/share/applications/org.xxxgram.desktop.desktop"
/usr/bin/python3 - <<'PY'
import os
from pathlib import Path
def quoted(value):
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$').replace('%', '%%') + '"'
Path(os.environ['XXXGRAM_DESKTOP']).write_text(
    '[Desktop Entry]\nType=Application\nName=xxxgram\nComment=Telegram с настройками xxxgram\n'
    'Exec=' + quoted(os.environ['XXXGRAM_LAUNCHER']) + ' -- %u\n'
    'Icon=' + os.environ['XXXGRAM_ICON'] + '\n'
    'Terminal=false\nCategories=Network;InstantMessaging;\nStartupWMClass=xxxgram\n'
    'MimeType=x-scheme-handler/tg;\n', encoding='utf-8')
PY
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$HOME/.local/share/applications"
fi
echo 'xxxgram установлен. Запуск: меню приложений → xxxgram.'
