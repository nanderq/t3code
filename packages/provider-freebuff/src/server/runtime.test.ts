import { assert, describe, it } from "@effect/vitest";
import * as Deferred from "effect/Deferred";
import * as Effect from "effect/Effect";
import * as Fiber from "effect/Fiber";
import * as Queue from "effect/Queue";
import * as Stream from "effect/Stream";
import type * as AcpSessionRuntime from "@t3tools/provider-acp/server/AcpSessionRuntime";
import { makeFreebuffRuntime } from "./runtime.ts";

describe("Freebuff ACP runtime", () => {
  it.effect("retains Stop received before the native prompt is dispatched", () =>
    Effect.gen(function* () {
      let nativePrompts = 0;
      const runtime = yield* makeFreebuffRuntime({
        binaryPath: "freebuff",
        cwd: "/workspace",
        environment: {},
        stateDirectory: "/state/freebuff",
        createSession: async () => ({
          sessionId: "conversation-1",
          prompt: async ({ signal }) => {
            nativePrompts++;
            return { cancelled: signal.aborted };
          },
          close: async () => {},
        }),
      });
      yield* runtime.getEvents().pipe(
        Stream.runForEach((event) =>
          event._tag === "EventStreamBarrier"
            ? Deferred.succeed(event.acknowledge, undefined)
            : Effect.void,
        ),
        Effect.forkScoped,
      );
      yield* runtime.cancel;
      const result = yield* runtime.prompt({ prompt: [{ type: "text", text: "Work" }] });
      assert.equal(result.stopReason, "cancelled");
      assert.equal(nativePrompts, 0);
      const next = yield* runtime.prompt({ prompt: [{ type: "text", text: "New work" }] });
      assert.equal(next.stopReason, "end_turn");
      assert.equal(nativePrompts, 1);
    }).pipe(Effect.scoped),
  );

  it.effect("projects structured assistant and tool snapshots without duplicating text", () =>
    Effect.gen(function* () {
      const runtime = yield* makeFreebuffRuntime({
        binaryPath: "freebuff",
        cwd: "/workspace",
        environment: {},
        stateDirectory: "/state/freebuff",
        createSession: async () => ({
          sessionId: "conversation-1",
          prompt: async ({ onEvent }) => {
            onEvent({ type: "text", id: "reply", text: "Hello", complete: false });
            onEvent({ type: "text", id: "reply", text: "Hello world", complete: false });
            onEvent({ type: "text", id: "reply", text: "Hello world", complete: true });
            onEvent({
              type: "tool",
              id: "tool",
              name: "read_files",
              input: { paths: ["a.ts"] },
              output: "contents",
              status: "completed",
            });
            return { cancelled: false };
          },
          close: async () => {},
        }),
      });
      const received = yield* Queue.unbounded<AcpSessionRuntime.AcpSessionRuntimeEvent>();
      yield* runtime.getEvents().pipe(
        Stream.runForEach((event) =>
          event._tag === "EventStreamBarrier"
            ? Deferred.succeed(event.acknowledge, undefined)
            : Queue.offer(received, event),
        ),
        Effect.forkScoped,
      );
      const started = yield* runtime.start();
      assert.equal(started.sessionId, "conversation-1");
      const result = yield* runtime.prompt({ prompt: [{ type: "text", text: "Hello" }] });
      assert.equal(result.stopReason, "end_turn");
      const events = yield* Queue.takeN(received, 5);
      assert.deepEqual(
        events.map((event) => event._tag),
        [
          "AssistantItemStarted",
          "ContentDelta",
          "ContentDelta",
          "AssistantItemCompleted",
          "ToolCallUpdated",
        ],
      );
      assert.deepEqual(
        events.filter((event) => event._tag === "ContentDelta").map((event) => event.text),
        ["Hello", " world"],
      );
      const tool = events.find((event) => event._tag === "ToolCallUpdated");
      assert.equal(tool?.toolCall.status, "completed");
      assert.equal(tool?.toolCall.detail, "contents");
    }).pipe(Effect.scoped),
  );

  it.effect("cancels the active native prompt and retains the conversation", () =>
    Effect.gen(function* () {
      const dispatched = yield* Deferred.make<void>();
      const runtime = yield* makeFreebuffRuntime({
        binaryPath: "freebuff",
        cwd: "/workspace",
        environment: {},
        stateDirectory: "/state/freebuff",
        resumeSessionId: "resumed-conversation",
        createSession: async (options) => ({
          sessionId: options.resumeSessionId!,
          prompt: ({ signal }) =>
            new Promise((resolve) => {
              signal.addEventListener("abort", () => resolve({ cancelled: true }), { once: true });
            }),
          close: async () => {},
        }),
      });
      yield* runtime.getEvents().pipe(
        Stream.runForEach((event) =>
          event._tag === "EventStreamBarrier"
            ? Deferred.succeed(event.acknowledge, undefined)
            : Effect.void,
        ),
        Effect.forkScoped,
      );
      const prompt = yield* runtime
        .prompt({ prompt: [{ type: "text", text: "Work" }] }, { dispatched })
        .pipe(Effect.forkScoped);
      yield* Deferred.await(dispatched);
      yield* runtime.cancel;
      assert.equal((yield* Fiber.join(prompt)).stopReason, "cancelled");
      assert.equal((yield* runtime.start()).sessionId, "resumed-conversation");
    }).pipe(Effect.scoped),
  );
});
