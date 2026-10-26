import * as NodeServices from "@effect/platform-node/NodeServices";
import { assert, describe, it } from "@effect/vitest";
import {
  EnvironmentId,
  MessageId,
  NodeId,
  ProjectId,
  ProviderInstanceId,
  ProviderSessionId,
  RunAttemptId,
  RunId,
  ThreadId,
  type OrchestrationV2ProviderThread,
} from "@t3tools/contracts";
import * as IdAllocator from "@t3tools/provider-core/server/IdAllocator";
import * as McpProviderSessions from "@t3tools/provider-core/server/McpProviderSessions";
import * as ProviderAdapter from "@t3tools/provider-core/server/ProviderAdapter";
import * as TestProviderHost from "@t3tools/provider-testing/TestProviderHost";
import * as DateTime from "effect/DateTime";
import * as Deferred from "effect/Deferred";
import * as Effect from "effect/Effect";
import * as FileSystem from "effect/FileSystem";
import * as Layer from "effect/Layer";
import * as Result from "effect/Result";
import * as Schema from "effect/Schema";
import * as Stream from "effect/Stream";
import { FreebuffSettings } from "../settings.ts";
import { makeFreebuffAdapterV2 } from "./adapter.ts";

const testLayer = Layer.mergeAll(
  NodeServices.layer,
  IdAllocator.layer,
  McpProviderSessions.layer,
  TestProviderHost.layer().pipe(Layer.provide(NodeServices.layer)),
);
const INSTANCE_ID = ProviderInstanceId.make("freebuff_test");
const THREAD_ID = ThreadId.make("thread-freebuff-test");
const SESSION_ID = ProviderSessionId.make("session-freebuff-test");
const modelSelection = { instanceId: INSTANCE_ID, model: "freebuff", options: [] };
const settings = Schema.decodeSync(FreebuffSettings)({ binaryPath: "fake-freebuff" });
const fullAccess = ProviderAdapter.ProviderAdapterV2RuntimePolicy.make({
  runtimeMode: "full-access",
  interactionMode: "default",
  cwd: null,
});
type CreateSession = NonNullable<Parameters<typeof makeFreebuffAdapterV2>[0]["createSession"]>;
type CliSession = Awaited<ReturnType<CreateSession>>;

function fakeCli(prompt: CliSession["prompt"] = async () => ({ cancelled: false })) {
  const starts: Parameters<CreateSession>[0][] = [];
  const prompts: Parameters<CliSession["prompt"]>[0][] = [];
  let closes = 0;
  const createSession: CreateSession = async (options) => {
    starts.push(options);
    return {
      sessionId: options.resumeSessionId ?? "native-freebuff-conversation",
      prompt: (input) => {
        prompts.push(input);
        return prompt(input);
      },
      close: async () => {
        closes += 1;
      },
    };
  };
  return { createSession, starts, prompts, closes: () => closes };
}

const makeHarness = Effect.fn("FreebuffAdapter.testHarness")(function* (
  fake: ReturnType<typeof fakeCli>,
  options: {
    readonly policy?: ProviderAdapter.ProviderAdapterV2RuntimePolicy;
    readonly initialNativeThreadId?: string;
  } = {},
) {
  const fileSystem = yield* FileSystem.FileSystem;
  const cwd = yield* fileSystem.makeTempDirectoryScoped({ prefix: "freebuff-adapter-test-" });
  const policy = { ...(options.policy ?? fullAccess), cwd };
  const adapter = yield* makeFreebuffAdapterV2({
    instanceId: INSTANCE_ID,
    settings,
    environment: { FREEBUFF_TEST: "isolated" },
    createSession: fake.createSession,
  });
  const runtime = yield* adapter.openSession({
    threadId: THREAD_ID,
    providerSessionId: SESSION_ID,
    modelSelection,
    runtimePolicy: policy,
    ...(options.initialNativeThreadId
      ? { initialNativeThreadId: options.initialNativeThreadId }
      : {}),
  });
  const providerThread = yield* runtime.ensureThread({
    threadId: THREAD_ID,
    modelSelection,
    runtimePolicy: policy,
  });
  return { adapter, runtime, providerThread, policy, cwd };
});

const turnInput = Effect.fn("FreebuffAdapter.testTurn")(function* (
  providerThread: OrchestrationV2ProviderThread,
  policy: ProviderAdapter.ProviderAdapterV2RuntimePolicy,
  ordinal = 1,
): Effect.fn.Return<ProviderAdapter.ProviderAdapterV2TurnInput> {
  const now = yield* DateTime.now;
  return {
    threadId: THREAD_ID,
    providerThread,
    runtimePolicy: policy,
    modelSelection,
    runId: RunId.make(`freebuff-run-${ordinal}`),
    runOrdinal: ordinal,
    providerTurnOrdinal: ordinal,
    attemptId: RunAttemptId.make(`freebuff-attempt-${ordinal}`),
    rootNodeId: NodeId.make(`freebuff-root-${ordinal}`),
    message: {
      messageId: MessageId.make(`freebuff-message-${ordinal}`),
      text: "Read a.ts and explain it.",
      attachments: [],
      createdBy: "user",
      creationSource: "web",
    },
    appThread: {
      id: THREAD_ID,
      projectId: ProjectId.make("freebuff-test-project"),
      title: "Freebuff test",
      createdBy: "user",
      creationSource: "web",
      providerInstanceId: INSTANCE_ID,
      modelSelection,
      runtimeMode: policy.runtimeMode,
      interactionMode: policy.interactionMode,
      branch: null,
      worktreePath: null,
      activeProviderThreadId: providerThread.id,
      lineage: { parentThreadId: null, relationshipToParent: null, rootThreadId: THREAD_ID },
      forkedFrom: null,
      createdAt: now,
      updatedAt: now,
      archivedAt: null,
      settledOverride: null,
      settledAt: null,
      lastVisitedAt: null,
      deletedAt: null,
    },
  };
});

const collectTurn = (runtime: ProviderAdapter.ProviderAdapterV2SessionRuntime) =>
  runtime.events.pipe(
    Stream.takeUntil((event) => event.type === "turn.terminal"),
    Stream.runCollect,
  );

describe("FreebuffAdapterV2", () => {
  it.effect("creates a normal thread and projects native responses and tool output", () =>
    Effect.gen(function* () {
      const fake = fakeCli(async ({ onEvent }) => {
        onEvent({ type: "text", id: "reply", text: "Hello", complete: false });
        onEvent({ type: "text", id: "reply", text: "Hello world", complete: true });
        onEvent({ type: "text", id: "reply", text: "Hello world", complete: true });
        onEvent({
          type: "tool",
          id: "read-a",
          name: "read_files",
          input: { paths: ["a.ts"] },
          status: "running",
        });
        onEvent({
          type: "tool",
          id: "read-a",
          name: "read_files",
          input: { paths: ["a.ts"] },
          output: "export const answer = 42;",
          status: "completed",
        });
        return { cancelled: false };
      });
      const { runtime, providerThread, policy, cwd } = yield* makeHarness(fake);
      assert.equal(runtime.providerSession.driver, "freebuff");
      assert.equal(runtime.providerSession.status, "ready");
      assert.equal(providerThread.providerInstanceId, INSTANCE_ID);
      assert.equal(providerThread.appThreadId, THREAD_ID);
      assert.equal(providerThread.nativeThreadRef?.nativeId, "native-freebuff-conversation");
      assert.equal(fake.starts[0]?.binaryPath, "fake-freebuff");
      assert.equal(fake.starts[0]?.cwd, cwd);
      assert.equal(fake.starts[0]?.environment.FREEBUFF_TEST, "isolated");

      yield* runtime.startTurn(yield* turnInput(providerThread, policy));
      const events = yield* collectTurn(runtime);
      assert.deepEqual(
        events
          .filter((event) => event.type === "provider_turn.updated")
          .map((event) => event.providerTurn.status),
        ["running", "completed"],
      );
      const reply = events.findLast(
        (event) => event.type === "message.updated" && event.message.role === "assistant",
      );
      assert(reply?.type === "message.updated");
      assert.equal(reply.message.text, "Hello world");
      assert.equal(reply.message.streaming, false);
      assert.equal(reply.message.threadId, THREAD_ID);
      assert.equal(
        new Set(
          events.flatMap((event) =>
            event.type === "message.updated" && event.message.role === "assistant"
              ? [event.message.id]
              : [],
          ),
        ).size,
        1,
      );
      const tool = events.findLast(
        (event) => event.type === "turn_item.updated" && event.turnItem.type === "dynamic_tool",
      );
      assert(tool?.type === "turn_item.updated" && tool.turnItem.type === "dynamic_tool");
      assert.equal(tool.turnItem.status, "completed");
      assert.equal(tool.turnItem.toolName, "Read");
      assert.deepEqual(tool.turnItem.input, { paths: ["a.ts"] });
      assert.equal(tool.turnItem.output, "export const answer = 42;");
      assert.equal(
        new Set(
          events.flatMap((event) =>
            event.type === "turn_item.updated" && event.turnItem.type === "dynamic_tool"
              ? [event.turnItem.id]
              : [],
          ),
        ).size,
        1,
      );
      const terminal = events.at(-1);
      assert(terminal?.type === "turn.terminal");
      assert.equal(terminal.status, "completed");
      assert.equal(terminal.threadDisposition, "reusable");
      assert.equal(fake.prompts.length, 1);
      assert.include(fake.prompts[0]!.text, "Read a.ts and explain it.");
    }).pipe(Effect.scoped, Effect.provide(testLayer)),
  );

  it.effect("replaces a draft when the native final snapshot revises its text", () =>
    Effect.gen(function* () {
      const fake = fakeCli(async ({ onEvent }) => {
        onEvent({ type: "text", id: "reply", text: "Working draft", complete: false });
        onEvent({ type: "text", id: "reply", text: "Final answer", complete: true });
        return { cancelled: false };
      });
      const { runtime, providerThread, policy } = yield* makeHarness(fake);
      yield* runtime.startTurn(yield* turnInput(providerThread, policy));
      const events = yield* collectTurn(runtime);
      const reply = events.findLast(
        (event) => event.type === "message.updated" && event.message.role === "assistant",
      );
      assert(reply?.type === "message.updated");
      assert.equal(reply.message.text, "Final answer");
    }).pipe(Effect.scoped, Effect.provide(testLayer)),
  );

  it.effect("keeps its fixed capability limits after ACP initialization", () =>
    Effect.gen(function* () {
      const mcpSessions = yield* McpProviderSessions.McpProviderSessions;
      yield* mcpSessions.set({
        environmentId: EnvironmentId.make("freebuff-test-environment"),
        threadId: THREAD_ID,
        providerSessionId: SESSION_ID,
        providerInstanceId: INSTANCE_ID,
        endpoint: "http://127.0.0.1:43123/mcp",
        authorizationHeader: "Bearer synthetic-freebuff-token",
        browserToolsAvailable: true,
      });
      const fake = fakeCli();
      const { adapter, runtime, providerThread, policy } = yield* makeHarness(fake);
      const declared = yield* adapter.getCapabilities();
      const live = runtime.providerSession.capabilities;
      assert.deepEqual(live, declared);
      assert.equal(live.tools.supportsMcpTools, false);
      assert.equal(live.tools.supportsDynamicToolCallbacks, false);
      assert.equal(live.threads.canReadThreadSnapshot, false);
      assert.equal(live.threads.canForkThread, false);
      assert.equal(live.checkpointing.providerCanReadConversationSnapshot, false);
      assert.equal(live.sessions.supportsModelSwitchInSession, false);
      assert.equal(live.sessions.supportsRuntimeModeSwitchInSession, false);
      assert.equal(live.approvals.supportsCommandApproval, false);
      assert.equal(live.approvals.supportsFileReadApproval, false);
      assert.equal(live.approvals.supportsFileChangeApproval, false);
      assert.equal(live.streaming.streamsReasoning, false);
      assert.equal(live.planning.emitsPlanUpdated, false);
      assert.equal(live.planning.emitsTodoList, false);
      assert.equal(live.planning.supportsStructuredQuestions, false);
      yield* runtime.startTurn(yield* turnInput(providerThread, policy));
      yield* collectTurn(runtime);
      assert.notInclude(fake.prompts[0]!.text, "delegate_task");
      assert.notInclude(fake.prompts[0]!.text, "preview_");
    }).pipe(Effect.scoped, Effect.provide(testLayer)),
  );

  it.effect.each([
    { ...fullAccess, runtimeMode: "approval-required" as const },
    { ...fullAccess, runtimeMode: "auto-accept-edits" as const },
    { ...fullAccess, runtimeMode: "auto" as const },
    { ...fullAccess, approvalPolicy: "never" },
    { ...fullAccess, sandboxPolicy: "danger-full-access" },
  ])("rejects unsupported runtime policy before starting the CLI: %j", (policy) =>
    Effect.gen(function* () {
      const fake = fakeCli();
      const result = yield* makeHarness(fake, { policy }).pipe(Effect.result);
      assert(Result.isFailure(result));
      assert.equal(fake.starts.length, 0);
      assert.equal(result.failure._tag, "ProviderAdapterOpenSessionError");
      assert("cause" in result.failure);
      assert.include(String(result.failure.cause), "Freebuff supports Full access only");
    }).pipe(Effect.scoped, Effect.provide(testLayer)),
  );

  it.effect("reopens a saved native conversation and continues it", () =>
    Effect.gen(function* () {
      const fake = fakeCli();
      const { runtime, providerThread, policy } = yield* makeHarness(fake, {
        initialNativeThreadId: "saved-freebuff-conversation",
      });
      assert.equal(fake.starts[0]?.resumeSessionId, "saved-freebuff-conversation");
      assert.equal(providerThread.nativeThreadRef?.nativeId, "saved-freebuff-conversation");
      const resumed = yield* runtime.resumeThread({
        providerThread,
        modelSelection,
        runtimePolicy: policy,
      });
      assert.equal(resumed.id, providerThread.id);
      assert.equal(resumed.status, "idle");
      yield* runtime.startTurn(yield* turnInput(resumed, policy, 2));
      const events = yield* collectTurn(runtime);
      assert(
        events.some((event) => event.type === "turn.terminal" && event.status === "completed"),
      );
      assert.equal(fake.starts.length, 1);
      assert.equal(fake.prompts.length, 1);
    }).pipe(Effect.scoped, Effect.provide(testLayer)),
  );

  it.effect("stops an active turn and can run another turn on the same conversation", () =>
    Effect.gen(function* () {
      let cancelled = false;
      let promptCount = 0;
      const dispatched = yield* Deferred.make<void>();
      const fake = fakeCli(({ signal, onEvent }) => {
        if (promptCount++ > 0) {
          onEvent({ type: "text", id: "reply-after-stop", text: "Resumed", complete: true });
          return Promise.resolve({ cancelled: false });
        }
        return new Promise((resolve) => {
          signal.addEventListener(
            "abort",
            () => {
              cancelled = true;
              resolve({ cancelled: true });
            },
            { once: true },
          );
          Deferred.doneUnsafe(dispatched, Effect.void);
        });
      });
      const { runtime, providerThread, policy } = yield* makeHarness(fake);
      yield* runtime.startTurn(yield* turnInput(providerThread, policy));
      yield* Deferred.await(dispatched);
      const [started] = yield* runtime.events.pipe(
        Stream.filter((event) => event.type === "provider_turn.updated"),
        Stream.take(1),
        Stream.runCollect,
      );
      assert(started?.type === "provider_turn.updated");
      yield* runtime.interruptTurn({
        providerThread,
        providerTurnId: started.providerTurn.id,
        requestRuntimeRestart: true,
      });
      const events = yield* collectTurn(runtime);
      assert.equal(cancelled, true);
      assert(
        events.some((event) => event.type === "turn.terminal" && event.status === "interrupted"),
      );
      assert.equal(fake.closes(), 0);
      const resumed = yield* runtime.resumeThread({ providerThread });
      assert.equal(resumed.nativeThreadRef?.nativeId, providerThread.nativeThreadRef?.nativeId);
      yield* runtime.startTurn(yield* turnInput(resumed, policy, 2));
      const continued = yield* collectTurn(runtime);
      assert(
        continued.some((event) => event.type === "turn.terminal" && event.status === "completed"),
      );
      assert.equal(fake.starts.length, 1);
      assert.equal(fake.prompts.length, 2);
    }).pipe(Effect.scoped, Effect.provide(testLayer)),
  );
});
