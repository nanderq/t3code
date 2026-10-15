# nanderq/t3code fork

Upstream T3 Code nightly plus Discord Rich Presence
([pingdotgg/t3code#18346](https://github.com/pingdotgg/t3code/pull/18346)), built and
installed on a Mac every day.

## Branches

- `main` mirrors `pingdotgg/t3code` main.
- `discord-presence` holds the fork's changes. The nightly build merges it onto the
  latest upstream main, so keep it mergeable and push fixes here.

## Mac setup

1. Install Xcode Command Line Tools (`xcode-select --install`), Node 24, Rust
   (`rustup`), and [Vite+](https://viteplus.dev) (`vp`).
2. Create a Discord application at <https://discord.com/developers/applications>. Name it
   what you want friends to see (for example "T3 Code"). Under Rich Presence → Art Assets,
   upload images named `nightly`, `black`, and `blueprint`. Copy the Application ID.
3. Clone into a directory used only for these builds. Every run hard-resets it.

   ```sh
   git clone -b discord-presence https://github.com/nanderq/t3code ~/t3code-fork
   ~/t3code-fork/fork/setup-mac.sh
   ```

4. Put the Application ID in `~/Library/Application Support/t3code-fork/config.env`
   (`DISCORD_CLIENT_ID=...`).
5. Build once: `~/t3code-fork/fork/nightly-mac.sh --force`.

The build runs daily at 05:00, and only when upstream main or `discord-presence` has
changed. It installs `/Applications/T3 Code (Nightly).app`, which replaces an upstream
Nightly install and keeps its data. If the app is open, the build waits until a later run
after you quit it. Logs: `~/Library/Logs/t3code-fork-nightly.log`.

## Updates

These builds are unsigned, so they ship without an in-app update feed: macOS rejects
unsigned updates, and leaving the feed out also stops upstream nightlies from replacing
the fork. The daily job is the updater. In-app updates from this fork's releases need an
Apple Developer ID.

## Enable Discord Rich Presence

Settings → Integrations → Discord. It is off by default. Project name, model, activity,
and elapsed time can each be turned on or off.
