// @effect-diagnostics nodeBuiltinImport:off globalTimers:off -- Native PTY and filesystem callbacks form the Promise boundary consumed by the Effect session runtime.
import * as NodeCrypto from "node:crypto";
import * as NodeFS from "node:fs";
import * as NodeFSP from "node:fs/promises";
import * as NodeModule from "node:module";
import * as NodeOS from "node:os";
import * as NodePath from "node:path";
import * as NodeProcess from "node:process";
import * as NodeUtil from "node:util";

import type { PtyProcess, PtySpawnInput } from "@t3tools/shared/PtyAdapter";

import {
  decodeFreebuffMessages,
  isRecord,
  projectFreebuffEvents,
  readFreebuffLogLines,
  type FreebuffCliEvent,
  type FreebuffMessage,
} from "./protocol.ts";

export type { FreebuffCliEvent } from "./protocol.ts";

export interface FreebuffCliSession {
  readonly sessionId: string;
  readonly prompt: (input: {
    readonly text: string;
    readonly signal: AbortSignal;
    readonly onEvent: (event: FreebuffCliEvent) => void;
  }) => Promise<{ readonly cancelled: boolean }>;
  readonly close: () => Promise<void>;
}

interface StartOptions {
  readonly binaryPath: string;
  readonly cwd: string;
  readonly environment: NodeJS.ProcessEnv;
  readonly stateDirectory: string;
  readonly resumeSessionId?: string;
  readonly spawn?: (input: PtySpawnInput) => Promise<PtyProcess>;
}

interface Turn {
  readonly onEvent: (event: FreebuffCliEvent) => void;
  readonly baseline: Set<string>;
  readonly emitted: Map<string, string>;
  readonly resolve: (result: { cancelled: boolean }) => void;
  readonly reject: (error: Error) => void;
  readonly cleanUp: () => void;
  finished: boolean;
  started: boolean;
}

function deferred<A>() {
  const result = Promise.withResolvers<A>();
  // Acquisition may finish before the first prompt attaches its readiness listener.
  void result.promise.catch(() => undefined);
  return result;
}

async function readJson(file: string): Promise<unknown> {
  try {
    return JSON.parse(await NodeFSP.readFile(file, "utf8")) as unknown;
  } catch (error) {
    if (isRecord(error) && error.code === "ENOENT") return undefined;
    throw error;
  }
}

function escapeWindowsArgument(value: string): string {
  const quoted = `"${value.replace(/(\\*)"/g, '$1$1\\"').replace(/(\\*)$/, "$1$1")}"`;
  return quoted.replace(/([()\][%!^"`<>&|;, *?])/g, "^$1");
}

const requireForPty = NodeModule.createRequire(import.meta.url);

async function spawnPty(input: PtySpawnInput): Promise<PtyProcess> {
  // The native addon must remain external to the server bundle, including Node SEA builds.
  const native = requireForPty("node-pty") as typeof import("node-pty");
  const child = native.spawn(input.shell, input.args ?? [], {
    cwd: input.cwd,
    env: input.env,
    cols: input.cols,
    rows: input.rows,
    name: "xterm-256color",
  });
  let processGroup: number | undefined;
  try {
    const stat = NodeFS.readFileSync(`/proc/${child.pid}/stat`, "utf8");
    const fields = stat.slice(stat.lastIndexOf(")") + 2).split(" ");
    if (Number(fields[2]) === child.pid) processGroup = child.pid;
  } catch {
    // /proc is Linux-only; other hosts retain node-pty's native cleanup.
  }
  const killGroup = (signal: NodeJS.Signals) => {
    if (processGroup === undefined) return;
    try {
      NodeProcess.kill(-processGroup, signal);
    } catch (error) {
      if (!isRecord(error) || error.code !== "ESRCH") throw error;
    }
  };
  return {
    get pid() {
      return child.pid;
    },
    write: (data) => child.write(data),
    resize: (cols, rows) => child.resize(cols, rows),
    kill: (signal) => {
      if (processGroup !== undefined) killGroup((signal ?? "SIGHUP") as NodeJS.Signals);
      else child.kill(signal);
    },
    onData: (callback) => {
      const subscription = child.onData(callback);
      return () => subscription.dispose();
    },
    onExit: (callback) => {
      const subscription = child.onExit((event) => {
        // A shell/tool may ignore TERM after its CLI parent exits. This group
        // was verified against the PID captured from our own forkpty spawn.
        killGroup("SIGKILL");
        callback({ exitCode: event.exitCode, signal: event.signal ?? null });
      });
      return () => subscription.dispose();
    },
  };
}

export async function createFreebuffCliSession(options: StartOptions): Promise<FreebuffCliSession> {
  const sessionId = options.resumeSessionId ?? NodeCrypto.randomUUID();
  if (!/^[a-zA-Z0-9_-]+$/.test(sessionId)) throw new Error("Invalid Freebuff session id.");
  const sourceDirectory = NodePath.resolve(
    options.environment.FREEBUFF_CONFIG_DIR ??
      NodePath.join(
        options.environment.HOME ?? options.environment.USERPROFILE ?? NodeOS.homedir(),
        ".config",
        "manicode",
      ),
  );
  const configDirectory = NodePath.resolve(options.stateDirectory, sessionId);
  const relative = NodePath.relative(sourceDirectory, configDirectory);
  if (relative === "" || (!relative.startsWith("..") && !NodePath.isAbsolute(relative))) {
    throw new Error("Freebuff session state must be isolated from the normal CLI configuration.");
  }
  await NodeFSP.mkdir(configDirectory, { recursive: true, mode: 0o700 });
  const metadataPath = NodePath.join(configDirectory, "t3-session.json");
  const metadata = await readJson(metadataPath);
  if (
    options.resumeSessionId &&
    (!isRecord(metadata) || metadata.cwd !== options.cwd || typeof metadata.chatId !== "string")
  ) {
    throw new Error("The saved Freebuff session is unavailable for this workspace.");
  }
  let chatId =
    isRecord(metadata) && typeof metadata.chatId === "string" ? metadata.chatId : undefined;
  const credentials = await readJson(NodePath.join(sourceDirectory, "credentials.json"));
  const profile =
    isRecord(credentials) && isRecord(credentials.default) ? credentials.default : null;
  if (!profile?.authToken && !options.environment.CODEBUFF_API_KEY) {
    throw new Error("Sign in with `freebuff login` before starting a Freebuff thread.");
  }
  for (const name of ["credentials.json", "device-key.json", "settings.json"]) {
    try {
      const target = NodePath.join(configDirectory, name);
      await NodeFSP.copyFile(NodePath.join(sourceDirectory, name), target);
      await NodeFSP.chmod(target, 0o600);
    } catch (error) {
      if (!isRecord(error) || error.code !== "ENOENT") throw error;
    }
  }

  // The npm launcher installs its native binary independently of FREEBUFF_CONFIG_DIR.
  let binaryPath = options.binaryPath;
  if (binaryPath === "freebuff") {
    const nativePath = NodePath.join(
      NodeOS.homedir(),
      ".config",
      "manicode",
      options.environment.OS === "Windows_NT" ? "freebuff.exe" : "freebuff",
    );
    try {
      await NodeFSP.access(nativePath, NodeFS.constants.X_OK);
      binaryPath = nativePath;
    } catch {
      // Standalone PATH installations need no npm-managed native binary.
    }
  }

  const chatsDirectory = NodePath.join(
    configDirectory,
    "projects",
    NodePath.basename(options.cwd),
    "chats",
  );
  await NodeFSP.mkdir(chatsDirectory, { recursive: true });
  let closed = false;
  let child: PtyProcess | undefined;
  let ready = deferred<void>();
  let exited = deferred<void>();
  let turn: Turn | undefined;
  let startupTimer: ReturnType<typeof setTimeout> | undefined;
  let composerTimer: ReturnType<typeof setTimeout> | undefined;
  let enterTimer: ReturnType<typeof setTimeout> | undefined;
  let admissionTimer: ReturnType<typeof setTimeout> | undefined;
  let removeData: (() => void) | undefined;
  let removeExit: (() => void) | undefined;
  let chatWatcher: NodeFS.FSWatcher | undefined;
  let messages: readonly FreebuffMessage[] = [];
  let transcriptVersion: string | undefined;
  let logOffset = 0;
  let logRemainder = "";
  let scan = Promise.resolve();

  const fail = (error: Error) => {
    ready.reject(error);
    const current = turn;
    if (current) {
      turn = undefined;
      current.cleanUp();
      current.reject(error);
    }
  };

  const readSnapshot = async () => {
    if (!chatId) return;
    const chatDirectory = NodePath.join(chatsDirectory, chatId);
    const transcriptPath = NodePath.join(chatDirectory, "chat-messages.json");
    try {
      const stat = await NodeFSP.stat(transcriptPath);
      const version = `${stat.ino}:${stat.size}:${stat.mtimeMs}`;
      // Debug log writes are much more frequent than transcript checkpoints.
      if (version !== transcriptVersion) {
        const value = await readJson(transcriptPath);
        if (value !== undefined) {
          messages = decodeFreebuffMessages(value);
          transcriptVersion = version;
        }
      }
    } catch (error) {
      if (!isRecord(error) || error.code !== "ENOENT") throw error;
    }
    const current = turn;
    if (!current) return;
    const fresh = messages.filter((message) => !current.baseline.has(message.id));
    if (fresh.some((message) => message.variant === "user")) {
      current.started = true;
      clearTimeout(admissionTimer);
    }
    for (const event of projectFreebuffEvents(fresh)) {
      const encoded = JSON.stringify(event);
      if (current.emitted.get(event.id) === encoded) continue;
      current.emitted.set(event.id, encoded);
      current.onEvent(event);
    }
    const logPath = NodePath.join(chatDirectory, "log.jsonl");
    try {
      const file = await NodeFSP.open(logPath, "r");
      try {
        const stat = await file.stat();
        if (stat.size < logOffset) logOffset = 0;
        if (stat.size > logOffset) {
          const bytes = Buffer.alloc(stat.size - logOffset);
          const { bytesRead } = await file.read(bytes, 0, bytes.length, logOffset);
          logOffset += bytesRead;
          const parsed = readFreebuffLogLines(logRemainder + bytes.toString("utf8", 0, bytesRead));
          logRemainder = parsed.remainder;
          if (parsed.entries.some((entry) => entry.msg === "Main prompt finished"))
            current.finished = true;
        }
      } finally {
        await file.close();
      }
    } catch (error) {
      if (!isRecord(error) || error.code !== "ENOENT") throw error;
    }
    // The root finish log precedes the CLI's final transcript rename.
    if (
      turn === current &&
      current.finished &&
      fresh.findLast((message) => message.variant === "ai")?.complete
    ) {
      const state = await readJson(NodePath.join(chatDirectory, "run-state.json"));
      if (turn !== current) return;
      turn = undefined;
      current.cleanUp();
      if (isRecord(state) && isRecord(state.output) && state.output.type === "error") {
        current.reject(new Error("The Freebuff CLI could not complete this turn."));
      } else {
        current.resolve({ cancelled: false });
      }
    }
  };

  const scanFiles = () => {
    scan = scan
      .then(async () => {
        if (closed) return;
        if (!chatId) {
          const entries = await NodeFSP.readdir(chatsDirectory, { withFileTypes: true });
          const found = entries.find((entry) => entry.isDirectory());
          if (!found) return;
          chatId = found.name;
          await NodeFSP.writeFile(metadataPath, JSON.stringify({ cwd: options.cwd, chatId }), {
            mode: 0o600,
          });
        }
        if (!chatWatcher) {
          chatWatcher = NodeFS.watch(NodePath.join(chatsDirectory, chatId), (_event, filename) => {
            if (
              filename === "chat-messages.json" ||
              filename === "run-state.json" ||
              filename === "log.jsonl"
            )
              scanFiles();
          });
          chatWatcher.on("error", () =>
            fail(new Error("Could not observe Freebuff conversation files.")),
          );
        }
        await readSnapshot();
      })
      .catch(() => fail(new Error("Could not read Freebuff conversation files.")));
    return scan;
  };
  const directoryWatcher = NodeFS.watch(chatsDirectory, () => {
    void scanFiles();
  });
  directoryWatcher.on("error", () =>
    fail(new Error("Could not observe Freebuff conversation files.")),
  );

  const stopProcess = async () => {
    clearTimeout(startupTimer);
    clearTimeout(composerTimer);
    clearTimeout(enterTimer);
    const ownChild = child;
    if (!ownChild) return;
    const exitPromise = exited.promise;
    const killTimer = setTimeout(() => {
      try {
        ownChild.kill("SIGKILL");
      } catch {
        /* The captured process may already have exited. */
      }
      exited.resolve();
    }, 3_000);
    try {
      ownChild.kill("SIGTERM");
      await exitPromise;
    } catch {
      // A completed CLI can disappear between the captured handle and kill.
    } finally {
      clearTimeout(killTimer);
      removeData?.();
      removeExit?.();
      child = undefined;
    }
  };

  const startProcess = async () => {
    ready = deferred<void>();
    exited = deferred<void>();
    composerTimer = undefined;
    let terminal = "";
    const args = [
      "--cwd",
      options.cwd,
      "--trust-agents",
      ...(chatId ? ["--continue", chatId] : []),
    ];
    const useCmd =
      options.environment.OS === "Windows_NT" &&
      (binaryPath === "freebuff" || /\.(?:cmd|bat)$/i.test(binaryPath));
    child = await (options.spawn ?? spawnPty)({
      shell: useCmd ? (options.environment.ComSpec ?? "cmd.exe") : binaryPath,
      args: useCmd
        ? ["/d", "/s", "/c", [binaryPath, ...args].map(escapeWindowsArgument).join(" ")]
        : args,
      cwd: options.cwd,
      cols: 120,
      rows: 40,
      env: {
        ...options.environment,
        TERM: "xterm-256color",
        COLORTERM: "truecolor",
        FREEBUFF_CONFIG_DIR: configDirectory,
      },
    });
    const ownChild = child;
    startupTimer = setTimeout(() => {
      fail(
        new Error(
          "Freebuff did not reach its native message composer. Open the CLI to finish setup.",
        ),
      );
      void stopProcess();
    }, 60_000);
    removeData = child.onData((data) => {
      if (data.includes("\x1b[6n")) ownChild.write("\x1b[1;1R");
      terminal = (terminal + data).slice(-32_768);
      const plain = NodeUtil.stripVTControlCharacters(terminal);
      if (
        composerTimer === undefined &&
        (plain.includes("Enter a coding task or / for commands") ||
          plain.includes("Add to the current task (/ for commands)"))
      ) {
        // The first composer paint precedes mounting its input effects.
        composerTimer = setTimeout(() => {
          clearTimeout(startupTimer);
          ready.resolve();
        }, 1_000);
      }
    });
    removeExit = child.onExit(() => {
      clearTimeout(startupTimer);
      clearTimeout(composerTimer);
      exited.resolve();
      if (child === ownChild) child = undefined;
      fail(new Error("The Freebuff CLI process exited."));
    });
    await scanFiles();
  };

  try {
    await startProcess();
  } catch (error) {
    directoryWatcher.close();
    chatWatcher?.close();
    await stopProcess();
    throw error;
  }

  return {
    sessionId,
    async prompt(input) {
      if (closed) throw new Error("The Freebuff session is closed.");
      if (turn) throw new Error("A Freebuff turn is already running.");
      if (input.signal.aborted) return { cancelled: true };
      if (!child) await startProcess();
      if (input.signal.aborted) {
        await stopProcess();
        return { cancelled: true };
      }
      const aborted = deferred<void>();
      const notifyAbort = () => aborted.resolve();
      input.signal.addEventListener("abort", notifyAbort, { once: true });
      try {
        const state = await Promise.race([
          ready.promise.then(() => "ready" as const),
          aborted.promise.then(() => "aborted" as const),
        ]);
        if (state === "aborted" || input.signal.aborted) {
          await stopProcess();
          return { cancelled: true };
        }
        await scanFiles();
        if (chatId) {
          try {
            logOffset = (await NodeFSP.stat(NodePath.join(chatsDirectory, chatId, "log.jsonl")))
              .size;
          } catch {
            logOffset = 0;
          }
        }
        logRemainder = "";
        const result = deferred<{ cancelled: boolean }>();
        const cancel = () => {
          const current = turn;
          if (!current) return;
          turn = undefined;
          current.cleanUp();
          // Terminating only our PTY also cancels admission and blocked native tools.
          // The next prompt reopens the persisted native chat in the same isolated config.
          void stopProcess().then(() => current.resolve({ cancelled: true }));
        };
        const cleanUp = () => {
          clearTimeout(enterTimer);
          clearTimeout(admissionTimer);
          input.signal.removeEventListener("abort", cancel);
        };
        turn = {
          onEvent: input.onEvent,
          baseline: new Set(messages.map((message) => message.id)),
          emitted: new Map(),
          resolve: result.resolve,
          reject: result.reject,
          cleanUp,
          finished: false,
          started: false,
        };
        input.signal.addEventListener("abort", cancel, { once: true });
        if (input.signal.aborted) cancel();
        else {
          const ownChild = child;
          if (!ownChild) throw new Error("The Freebuff CLI process exited.");
          ownChild.write(`\x1b[200~${input.text.replaceAll("\x1b", "")}\x1b[201~`);
          // OpenTUI consumes paste as one input event; Enter in the same PTY chunk is lost.
          enterTimer = setTimeout(() => ownChild.write("\r"), 100);
          admissionTimer = setTimeout(() => {
            fail(
              new Error(
                "Freebuff is waiting for native session setup or confirmation. Open the CLI to continue.",
              ),
            );
            void stopProcess();
          }, 60_000);
        }
        return await result.promise;
      } finally {
        input.signal.removeEventListener("abort", notifyAbort);
      }
    },
    async close() {
      if (closed) return;
      closed = true;
      directoryWatcher.close();
      chatWatcher?.close();
      fail(new Error("The Freebuff session is closed."));
      await stopProcess();
      await scan;
    },
  };
}
