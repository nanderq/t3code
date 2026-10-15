#!/usr/bin/env bash
# Builds T3 Code (Nightly) for macOS from upstream main plus this fork's
# Discord Rich Presence branch, then installs it into /Applications.
#
# Run from a dedicated clone of nanderq/t3code: every run hard-resets the
# checkout. launchd runs it daily (see fork/setup-mac.sh); run it by hand with
# `fork/nightly-mac.sh --force` to rebuild even when nothing changed.
#
# Everything lives in main() so bash has parsed the whole script before the
# checkout reset below rewrites this file on disk.
set -euo pipefail

main() {
  local repo
  repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  local state_dir="$HOME/Library/Application Support/t3code-fork"
  local config="$state_dir/config.env"
  local force=0
  [[ "${1:-}" == "--force" ]] && force=1

  mkdir -p "$state_dir"
  # shellcheck source=/dev/null
  [[ -f "$config" ]] && source "$config"
  local fork_branch="${FORK_BRANCH:-discord-presence}"
  local app_dir="${INSTALL_DIR:-/Applications}"
  local app_name="T3 Code (Nightly).app"

  notify() {
    echo "$1"
    osascript -e "display notification \"$1\" with title \"T3 Code fork nightly\"" >/dev/null 2>&1 || true
  }

  cd "$repo"
  git remote get-url upstream >/dev/null 2>&1 ||
    git remote add upstream https://github.com/pingdotgg/t3code.git
  git fetch --quiet upstream main
  git fetch --quiet origin "$fork_branch"

  local upstream_sha fork_sha build_key
  upstream_sha="$(git rev-parse upstream/main)"
  fork_sha="$(git rev-parse "origin/$fork_branch")"
  build_key="$upstream_sha+$fork_sha"

  # A staged build from an earlier run waits for the app to be closed.
  install_staged() {
    local staged="$state_dir/staged/$app_name"
    [[ -d "$staged" ]] || return 0
    if ps -axo command= | grep -qF "$app_dir/$app_name/Contents/MacOS/"; then
      notify "A new build is ready. Quit T3 Code (Nightly) to install it on the next run."
      return 0
    fi
    rm -rf "$app_dir/$app_name.old"
    [[ -d "$app_dir/$app_name" ]] && mv "$app_dir/$app_name" "$app_dir/$app_name.old"
    mv "$staged" "$app_dir/$app_name"
    rm -rf "$app_dir/$app_name.old"
    notify "Installed $(cat "$state_dir/staged.version")."
  }

  if [[ $force -eq 0 && "$(cat "$state_dir/last-build" 2>/dev/null)" == "$build_key" ]]; then
    install_staged
    echo "Upstream and $fork_branch unchanged; nothing to build."
    return 0
  fi

  git reset --quiet --hard
  git clean -fdq
  git checkout --quiet -B fork-nightly upstream/main
  if ! git merge --quiet --no-edit "origin/$fork_branch"; then
    git merge --abort || true
    notify "Merge conflict between upstream main and $fork_branch. Resolve it on the branch and push."
    return 1
  fi

  # Same version shape as upstream nightlies (`<next patch>-nightly.<date>.<n>`),
  # which also selects the Nightly product name, icons, and app identity.
  local base version
  base="$(node -p "const [a,b,c]=require('./apps/desktop/package.json').version.replace(/[-+].*/,'').split('.');[a,b,+c+1].join('.')")"
  version="$base-nightly.$(date -u +%Y%m%d).$(date -u +%s)"

  local arch
  arch="$(uname -m)"
  [[ "$arch" == "x86_64" ]] && arch="x64"

  # Public Clerk/relay config comes from .env.example, as in a dev checkout.
  # The Discord application id is always taken from config.env so this build
  # never publishes under someone else's Discord application.
  [[ -f .env ]] || cp .env.example .env
  export T3CODE_DISCORD_CLIENT_ID="${DISCORD_CLIENT_ID:-}"
  [[ -n "$T3CODE_DISCORD_CLIENT_ID" ]] ||
    echo "DISCORD_CLIENT_ID is not set in $config; Discord Rich Presence will be inert."

  echo "Building $version ($arch) from upstream ${upstream_sha:0:9} + $fork_branch ${fork_sha:0:9}"
  rm -rf release
  vp install --filter=@t3tools/desktop... --filter=t3... --filter=@t3tools/scripts...
  node scripts/update-release-package-versions.ts "$version"
  vp run build:desktop
  vp run dist:desktop:artifact --platform mac --target dmg --arch "$arch" \
    --build-version "$version" --skip-build

  local zip
  zip="$(find release -maxdepth 1 -name '*.zip' | head -n 1)"
  [[ -n "$zip" ]] || { notify "Build finished without a macOS zip."; return 1; }
  rm -rf "$state_dir/staged"
  mkdir -p "$state_dir/staged"
  ditto -x -k "$zip" "$state_dir/staged"
  echo "$version" >"$state_dir/staged.version"
  echo "$build_key" >"$state_dir/last-build"
  git reset --quiet --hard

  install_staged
}

main "$@"
