#!/usr/bin/env bash
# One-time macOS setup: writes the config template and installs a launchd
# agent that runs fork/nightly-mac.sh every day at 05:00.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
state_dir="$HOME/Library/Application Support/t3code-fork"
config="$state_dir/config.env"
label="com.nanderq.t3code-fork-nightly"
plist="$HOME/Library/LaunchAgents/$label.plist"
log="$HOME/Library/Logs/t3code-fork-nightly.log"

for tool in git node vp cargo; do
  command -v "$tool" >/dev/null || { echo "Missing '$tool' on PATH; see fork/README.md." >&2; exit 1; }
done

mkdir -p "$state_dir" "$HOME/Library/LaunchAgents"
if [[ ! -f "$config" ]]; then
  cat >"$config" <<'EOF'
# Application ID of your own Discord application (discord.com/developers).
# Empty leaves Discord Rich Presence inert.
DISCORD_CLIENT_ID=
# Branch on nanderq/t3code that is merged on top of upstream main.
FORK_BRANCH=discord-presence
EOF
  echo "Wrote $config; set DISCORD_CLIENT_ID there."
fi

# launchd starts jobs with a bare PATH, so capture the one that has node/vp/cargo.
cat >"$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$label</string>
  <key>ProgramArguments</key>
  <array><string>/bin/bash</string><string>$repo/fork/nightly-mac.sh</string></array>
  <key>EnvironmentVariables</key>
  <dict><key>PATH</key><string>$PATH</string></dict>
  <key>StartCalendarInterval</key>
  <dict><key>Hour</key><integer>5</integer><key>Minute</key><integer>0</integer></dict>
  <key>StandardOutPath</key><string>$log</string>
  <key>StandardErrorPath</key><string>$log</string>
</dict>
</plist>
EOF

launchctl bootout "gui/$(id -u)" "$plist" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$plist"
echo "Installed $label. Logs: $log"
echo "Run a build now with: $repo/fork/nightly-mac.sh --force"
