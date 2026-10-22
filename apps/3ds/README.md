# T3 Code for Nintendo 3DS

A homebrew client for a 3DS running Luma3DS. It connects to a T3 Code machine on your
network, so you can watch and steer agents from the couch. It does not run agents itself.

- **Threads**: the list sits on the touch screen, with pinned threads first, then threads
  waiting on you and running ones. Pins and unread dots mark the rows. The top screen shows
  the selected thread. Open a thread to read its recent conversation, reply, stop a run,
  approve or decline a request. **More** lets you pin, archive, rename, snooze, mark unread
  or done, change the model and the plan and access modes, allow a request for the session,
  or dismiss a question that accepts a message reply. Always allow appears only when the
  request offers it. **Subagents** lists the agents a thread started; open one to read its
  work, and B returns to the parent. Subagents take no replies. **Fork** copies the
  conversation at its latest stable point, and **Changes** shows the workspace's diff against the branch base. **+ New** picks a
  project, a model (the default reuses the project's newest thread), and then the project
  folder or a new worktree.
- **Alerts**: a short sound and a toast tell you when a thread starts waiting on you. The
  sound needs the console's DSP firmware dump; the toast works without it.
- **Snooze**: hide a thread for 1 hour, 3 hours, or until tomorrow at 9:00 on the console
  clock. **Unsnooze** wakes it early.
- **Usage**: how much of each provider limit is left on top, 30 days of spend and token
  totals below.
- **PRs**: open pull requests across the machine's projects, with checks, review state, and
  branches. **To review** narrows the list to PRs that ask for your review. Open a PR to
  read its description and checks, approve it, comment, merge, or mark it ready.
- **Tasks**: run scheduled tasks and enable or disable their schedules.
- **Hosts**: pair, switch between, and forget machines.

## Build

You need devkitARM with libctru, and citro2d. The devkitPro Docker image has everything:

```sh
cd apps/3ds
docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm make
```

With a local devkitPro install, run `make`. Either way the output is `t3code.3dsx`.

## Install

Copy `t3code.3dsx` to `/3ds/` on the SD card and start it from the Homebrew Launcher. While
the launcher is waiting on its network loader (press Y), you can also send the build over
Wi-Fi with `3dslink t3code.3dsx`.

To put T3 Code on the HOME Menu instead, build a CIA with `make cia`. It needs
[makerom](https://github.com/3DSGuy/Project_CTR/releases) and
[bannertool](https://github.com/carstene1ns/3ds-bannertool) on your PATH. Copy
`t3code.cia` to the SD card and install it with FBI. Pairings are shared with the
Homebrew Launcher version.

## Pair with a machine

The 3DS speaks plain HTTP, so it needs an address on your local network. T3 Connect and
HTTPS-only routes such as Tailscale Serve won't work from the console.

1. On the host, open **Settings → Connections**, turn on **Network access**, and create a
   pairing link. On a command-line host, start the server with `t3 serve --host <lan-ip>`
   and run `t3 pair`.
2. On the 3DS, open **Hosts**, press X, and point the outer camera at the link's QR code.
   Without a QR code, press X again to type the host address, such as `192.168.1.20:3773`,
   and then the 12-character code from the link (the part after `#token=`).

The console stores its session in `sdmc:/3ds/t3code/environments.json`. Sessions last 30
days; after that, pair again. Forgetting a machine on the 3DS doesn't revoke its session, so
also remove it under **Settings → Connections** on the host.

## Reading a thread

The top screen shows the conversation the way the web app does: your messages as bubbles,
answers as text, and plans, task lists, approvals, and errors as cards. Both your messages
and answers render markdown (headings, lists, code, tables). A finished turn folds its tool calls under "Worked for ...". Press
SELECT to show or hide those steps. The touch screen lists your messages; tap one to jump to
it. Scroll to the top and press Left to load an earlier page. The view keeps your place.
Once earlier history is loaded, go back and reopen the thread to return to live updates.

## Reviewing a PR

On **PRs**, press A to open the selected PR. Use the D-pad to scroll its description and
checks. A approves with a message, X comments, and Y merges or marks a draft ready. Merge
needs a second Y press to confirm. Actions depend on your host's permissions; PRs still
requiring review cannot be merged here. SELECT reloads the detail and B returns to the list.

## Scheduled tasks

On **Tasks**, A runs the selected task now, X enables or disables its schedule, and Y
refreshes the list. Tasks are created and edited on another device.

Tap **More** for thread actions; when no approval or question is waiting, A opens it too. Use
the D-pad and A, or tap an action; drag to see more, and B closes the menu. SELECT in the
thread list also shows archived threads, which you can open and unarchive through **More**.
Opening an unread thread clears its unread dot. The thread header shows its plan and access
modes.

When an agent asks questions, choose **Answer**. The top screen shows the question; use
D-pad and A to select answers on the touch screen. Multi-select questions let you toggle
several options. X types an answer when allowed, L/R moves between questions, and Submit
sends the completed form. B cancels.

A reply sent while the agent is working waits in its queue instead of steering it.
**More → Queue** opens queued messages. Use up/down to choose a message and left/right to
choose Cancel, Up, Down, Steer, or Edit, then A to apply. Steer sends it to the current run.
You can also tap an action. Long messages have a preview, but must be edited on another
device.

## Controls

| Input              | Action                                                                                                      |
| ------------------ | ----------------------------------------------------------------------------------------------------------- |
| L / R, or tap tabs | Switch tabs                                                                                                 |
| D-pad, Circle Pad  | Move the selection, or scroll a conversation                                                                |
| A                  | Open a thread, answer or approve, open More, run a task, or connect                                         |
| B                  | Back                                                                                                        |
| X                  | Reply, comment on a PR, toggle a task, or scan a pairing QR code                                            |
| Y                  | Stop, decline, refresh, merge / mark ready, or remove                                                       |
| SELECT             | Show or hide done, archived, and snoozed threads; in a thread, show or hide steps; on PRs, toggle To review |
| START              | Quit                                                                                                        |

Tap a row to select it, and tap it again to open it. Drag on the touch screen to scroll.

In **Changes**, pick a file on the touch screen or with the D-pad, then press A to read it.
The D-pad and Circle Pad scroll its diff; L/R switch files. A returns to the file list,
B goes back, and Y reloads. Long lines end in `...`. Large or omitted diffs stay available
on your other devices.

## Limits

- Conversations start with the latest window. Earlier pages load up to a limit of 320
  items or 1 MiB of retained text; a page that would exceed the limit is not added.
- PR descriptions are excerpts and check lists show up to 48 checks. Tasks show the first
  64 entries.
- Replies are text only. Attachments, terminals, and settings aren't available.
- Diffs are read only, with up to 128 files, 64 KB per file, and 256 KB of patch text total.
- Question forms support up to 8 questions with 12 options each. Larger forms or oversized
  fields need another device. Question attachments aren't available.
- The queue shows up to 32 messages. Steer requires a running agent that supports steering.
- The model picker shows up to 12 enabled provider instances and 96 models. Model options
  use provider defaults; the default new-thread choice preserves the newest thread's options
  and modes, falling back to the project default or first available model.
- Usage days follow the console clock's UTC offset, rounded to the hour.

QR codes are read with [quirc](https://github.com/dlbeer/quirc) (ISC license), vendored in
`source/quirc`.

## Protocol harness

`host/` builds the same protocol code for your computer, so you can test pairing, the
thread stream, usage, pull requests, and commands against a real server without a 3DS:

```sh
make -C apps/3ds/host
apps/3ds/host/hostcli pair 127.0.0.1:3773 <pairing-code>
apps/3ds/host/hostcli watch 127.0.0.1:3773 <token> [seconds]
apps/3ds/host/hostcli convo 127.0.0.1:3773 <token> <thread-id>
make -C apps/3ds/host parser-test
apps/3ds/host/parser-test
```

`make -C apps/3ds/host test` runs the offline parser and payload checks: history, tasks, PRs,
questions, the queue, diff previews, and workspace refs. None of them open a connection.

Point it at a development server with its own `--home-dir`, not the T3 Code install you are
using.

### Checking the screens without a 3DS

`host/retroshot.c` runs the `.3dsx` headlessly in [Azahar](https://github.com/azahar-emu/azahar)'s
libretro core with its software renderer. It plays a button script and saves frames, with
the top screen above the bottom one. It needs `azahar_libretro.dylib` (or `.so`) from an
Azahar release and `libretro.h` from
[libretro-common](https://github.com/libretro/libretro-common/tree/master/include) placed in
`host/`. The comment at the top of the file describes the script format.

```sh
make -C apps/3ds/host retroshot
apps/3ds/host/retroshot azahar_libretro.dylib apps/3ds/t3code.3dsx script.txt
```
