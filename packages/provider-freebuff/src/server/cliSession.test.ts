// @effect-diagnostics nodeBuiltinImport:off -- These fixtures exercise the native callback boundary with real isolated files.
import * as NodeFSP from "node:fs/promises";
import * as NodeOS from "node:os";
import * as NodePath from "node:path";

import { describe, expect, it, vi } from "@effect/vitest";
import type { PtyExitEvent, PtyProcess, PtySpawnInput } from "@t3tools/shared/PtyAdapter";

import {
  createFreebuffCliSession,
  type FreebuffCliEvent,
  type FreebuffCliSession,
} from "./cliSession.ts";

function fakeTimers() {
  vi.useFakeTimers({ toFake: ["setTimeout", "clearTimeout"] });
  return { [Symbol.dispose]: () => vi.useRealTimers() };
}

async function writeSnapshot(file: string, value: unknown) {
  await NodeFSP.writeFile(`${file}.tmp`, JSON.stringify(value));
  await NodeFSP.rename(`${file}.tmp`, file);
}

async function fixture() {
  const root = await NodeFSP.mkdtemp(NodePath.join(NodeOS.tmpdir(), "t3-freebuff-driver-"));
  const source = NodePath.join(root, "source");
  const cwd = NodePath.join(root, "workspace");
  await NodeFSP.mkdir(source);
  await NodeFSP.mkdir(cwd);
  const credentialText = JSON.stringify({ default: { authToken: "test-only-token" } });
  await NodeFSP.writeFile(NodePath.join(source, "credentials.json"), credentialText);
  const spawns: PtySpawnInput[] = [];
  const sessions: FreebuffCliSession[] = [];
  const children: ReturnType<typeof makeChild>[] = [];
  function makeChild(input: PtySpawnInput) {
    const data = new Set<(value: string) => void>();
    const exits = new Set<(value: PtyExitEvent) => void>();
    const pasted = Promise.withResolvers<string>();
    const entered = Promise.withResolvers<void>();
    const writes: string[] = [];
    const signals: Array<string | undefined> = [];
    const process: PtyProcess = {
      pid: 900_000_001,
      write(value) {
        writes.push(value);
        if (value.includes("\x1b[200~")) pasted.resolve(value);
        if (value === "\r") entered.resolve();
      },
      resize() {},
      kill(signal) {
        signals.push(signal);
        for (const onExit of exits) onExit({ exitCode: 0, signal: null });
      },
      onData(callback) {
        data.add(callback);
        return () => data.delete(callback);
      },
      onExit(callback) {
        exits.add(callback);
        return () => exits.delete(callback);
      },
    };
    return {
      process,
      input,
      signals,
      writes,
      pasted: pasted.promise,
      entered: entered.promise,
      ready: () => {
        for (const callback of data) callback("Enter a coding task or / for commands");
      },
      chatDirectory: NodePath.join(
        input.env.FREEBUFF_CONFIG_DIR!,
        "projects",
        "workspace",
        "chats",
        "native-chat",
      ),
    };
  }
  const options = {
    binaryPath: "/fixture/freebuff",
    cwd,
    stateDirectory: NodePath.join(root, "state"),
    environment: { FREEBUFF_CONFIG_DIR: source, PATH: "/fixture/bin" },
    async spawn(input: PtySpawnInput) {
      spawns.push(input);
      const child = makeChild(input);
      children.push(child);
      await NodeFSP.mkdir(child.chatDirectory, { recursive: true });
      try {
        await NodeFSP.writeFile(NodePath.join(child.chatDirectory, "chat-messages.json"), "[]", {
          flag: "wx",
        });
        await NodeFSP.writeFile(NodePath.join(child.chatDirectory, "log.jsonl"), "", {
          flag: "wx",
        });
      } catch (error) {
        if (!(error instanceof Error) || !("code" in error) || error.code !== "EEXIST") throw error;
      }
      return child.process;
    },
  };
  return {
    options,
    source,
    credentialText,
    spawns,
    children,
    async create(resumeSessionId?: string) {
      const session = await createFreebuffCliSession({
        ...options,
        ...(resumeSessionId ? { resumeSessionId } : {}),
      });
      sessions.push(session);
      return session;
    },
    async [Symbol.asyncDispose]() {
      for (const session of sessions) await session.close();
      await NodeFSP.rm(root, { recursive: true, force: true });
    },
  };
}

describe("Freebuff native CLI session", () => {
  it("waits for the final root snapshot after the finish log and emits full snapshots", async () => {
    using _timers = fakeTimers();
    await using state = await fixture();
    const session = await state.create();
    const child = state.children[0]!;
    const events: FreebuffCliEvent[] = [];
    const incomplete = Promise.withResolvers<void>();
    let settled = false;
    const result = session
      .prompt({
        text: "Hello\nworld",
        signal: new AbortController().signal,
        onEvent(event) {
          events.push(event);
          if (event.type === "text" && event.text === "Partial") incomplete.resolve();
        },
      })
      .then((value) => {
        settled = true;
        return value;
      });
    child.ready();
    await vi.advanceTimersByTimeAsync(1_000);
    expect(await child.pasted).toBe("\x1b[200~Hello\nworld\x1b[201~");
    await vi.advanceTimersByTimeAsync(100);
    await child.entered;
    const user = { id: "user", variant: "user", content: "Hello\nworld" };
    const intermediate = {
      id: "intermediate",
      variant: "ai",
      content: "Earlier",
      isComplete: true,
    };
    await NodeFSP.appendFile(
      NodePath.join(child.chatDirectory, "log.jsonl"),
      '{"msg":"Main prompt finished"}\n',
    );
    await writeSnapshot(NodePath.join(child.chatDirectory, "chat-messages.json"), [
      user,
      intermediate,
      { id: "assistant", variant: "ai", content: "Partial", isComplete: false },
    ]);
    await Promise.race([
      incomplete.promise,
      result.then(() => {
        throw new Error("Turn completed before its final snapshot.");
      }),
    ]);
    expect(settled).toBe(false);
    await writeSnapshot(NodePath.join(child.chatDirectory, "run-state.json"), {
      output: { type: "lastMessage" },
    });
    await writeSnapshot(NodePath.join(child.chatDirectory, "chat-messages.json"), [
      user,
      intermediate,
      { id: "assistant", variant: "ai", content: "Final answer", isComplete: true },
    ]);
    expect(await result).toEqual({ cancelled: false });
    expect(events.at(-1)).toEqual({
      type: "text",
      id: "assistant",
      text: "Final answer",
      complete: true,
    });
    expect(await NodeFSP.readFile(NodePath.join(state.source, "credentials.json"), "utf8")).toBe(
      state.credentialText,
    );
    expect(child.input.env.FREEBUFF_CONFIG_DIR).not.toBe(state.source);
  });

  it("reopens only its saved native chat and refreshes copied credentials", async () => {
    await using state = await fixture();
    const original = await state.create();
    await original.close();
    const updated = JSON.stringify({ default: { authToken: "updated-test-token" } });
    await NodeFSP.writeFile(NodePath.join(state.source, "credentials.json"), updated);
    const resumed = await state.create(original.sessionId);
    expect(resumed.sessionId).toBe(original.sessionId);
    expect(state.spawns[1]?.args).toEqual([
      "--cwd",
      state.options.cwd,
      "--trust-agents",
      "--continue",
      "native-chat",
    ]);
    expect(
      await NodeFSP.readFile(
        NodePath.join(state.spawns[1]!.env.FREEBUFF_CONFIG_DIR!, "credentials.json"),
        "utf8",
      ),
    ).toBe(updated);
  });

  it("cancels its captured PTY and can reopen the same chat for the next prompt", async () => {
    using _timers = fakeTimers();
    await using state = await fixture();
    const session = await state.create();
    const child = state.children[0]!;
    const controller = new AbortController();
    const result = session.prompt({ text: "Long task", signal: controller.signal, onEvent() {} });
    child.ready();
    await vi.advanceTimersByTimeAsync(1_000);
    await child.pasted;
    controller.abort();
    expect(await result).toEqual({ cancelled: true });
    expect(child.signals).toEqual(["SIGTERM"]);
    expect(child.writes).not.toContain("\r");
    const nextController = new AbortController();
    const next = session.prompt({ text: "Next task", signal: nextController.signal, onEvent() {} });
    // Aborting while the next process starts also settles readiness without submitting a draft.
    nextController.abort();
    await vi.advanceTimersByTimeAsync(1_000);
    expect(await next).toEqual({ cancelled: true });
    expect(state.spawns[1]?.args).toContain("native-chat");
  });

  it("does not start a login flow when copied credentials are missing", async () => {
    await using state = await fixture();
    await NodeFSP.rm(NodePath.join(state.source, "credentials.json"));
    await expect(state.create()).rejects.toThrow("freebuff login");
    expect(state.spawns).toEqual([]);
  });
});
