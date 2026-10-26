import * as Deferred from "effect/Deferred";
import * as Effect from "effect/Effect";
import * as Queue from "effect/Queue";
import * as Ref from "effect/Ref";
import * as Semaphore from "effect/Semaphore";
import * as Stream from "effect/Stream";
import * as AcpErrors from "effect-acp/errors";
import type * as AcpSchema from "effect-acp/compat";
import * as AcpSessionRuntime from "@t3tools/provider-acp/server/AcpSessionRuntime";
import { createFreebuffCliSession, type FreebuffCliEvent } from "./cliSession.ts";

type FreebuffRuntimeOptions = Parameters<typeof createFreebuffCliSession>[0] & {
  readonly createSession?: typeof createFreebuffCliSession;
};

const INITIALIZE = {
  protocolVersion: 1,
  agentInfo: { name: "t3-freebuff", version: "1.0.0" },
  agentCapabilities: {
    loadSession: true,
    promptCapabilities: { image: false, audio: false, embeddedContext: true },
  },
} satisfies AcpSchema.InitializeResponse;

const unsupported = (method: string) =>
  Effect.fail(AcpErrors.AcpRequestError.methodNotFound(method));

/** The native CLI owns execution; ACP supplies T3's existing timeline and turn lifecycle. */
export const makeFreebuffRuntime = Effect.fn("FreebuffRuntime.make")(function* (
  options: FreebuffRuntimeOptions,
) {
  const events = yield* Queue.unbounded<AcpSessionRuntime.AcpSessionRuntimeEvent>();
  const notifications = yield* Queue.unbounded<
    | { readonly type: "update"; readonly notification: AcpSchema.SessionNotification }
    | { readonly type: "barrier"; readonly acknowledge: Deferred.Deferred<void> }
  >();
  const updateHandlers: Array<
    Parameters<AcpSessionRuntime.AcpSessionRuntime["Service"]["handleSessionUpdate"]>[0]
  > = [];
  const notificationFailure = yield* Ref.make<AcpErrors.AcpError | undefined>(undefined);
  yield* Stream.fromQueue(notifications).pipe(
    Stream.runForEach((entry) =>
      entry.type === "barrier"
        ? Deferred.succeed(entry.acknowledge, undefined)
        : Effect.forEach(updateHandlers, (handler) => handler(entry.notification), {
            discard: true,
          }).pipe(
            Effect.catchCause((cause) =>
              Ref.set(
                notificationFailure,
                AcpErrors.AcpRequestError.internalError(
                  "Freebuff transcript projection failed.",
                  undefined,
                  { cause },
                ),
              ),
            ),
          ),
    ),
    Effect.forkScoped,
  );
  const promptLock = yield* Semaphore.make(1);
  const session = yield* Effect.acquireRelease(
    Effect.tryPromise({
      try: () => (options.createSession ?? createFreebuffCliSession)(options),
      catch: (cause) => new AcpErrors.AcpSpawnError({ command: options.binaryPath, cause }),
    }),
    (session) => Effect.promise(() => session.close()).pipe(Effect.ignoreCause({ log: true })),
  );
  let activePrompt: AbortController | undefined;
  let stopPending = false;
  let closed = false;
  const textById = new Map<string, string>();
  const completedText = new Set<string>();
  const toolsById = new Map<string, string>();
  const notify = (update: AcpSchema.SessionUpdate) => {
    if (!closed)
      Queue.offerUnsafe(notifications, {
        type: "update",
        notification: { sessionId: session.sessionId, update },
      });
  };

  const offer = (event: AcpSessionRuntime.AcpSessionRuntimeEvent) => {
    if (!closed) Queue.offerUnsafe(events, event);
  };
  const onEvent = (event: FreebuffCliEvent) => {
    if (event.type === "text") {
      const previous = textById.get(event.id);
      if (previous === undefined) offer({ _tag: "AssistantItemStarted", itemId: event.id });
      // Persisted chat snapshots grow as the CLI streams. Repeated snapshots carry no new text.
      if (event.text.startsWith(previous ?? "")) {
        const text = event.text.slice(previous?.length ?? 0);
        if (text) {
          offer({ _tag: "ContentDelta", itemId: event.id, text, rawPayload: event });
          notify({
            sessionUpdate: "agent_message_chunk",
            messageId: event.id,
            content: { type: "text", text },
          });
        }
      } else {
        notify({
          sessionUpdate: "agent_message",
          messageId: event.id,
          content: [{ type: "text", text: event.text }],
        });
      }
      textById.set(event.id, event.text);
      if (event.complete && !completedText.has(event.id)) {
        completedText.add(event.id);
        offer({ _tag: "AssistantItemCompleted", itemId: event.id });
      }
      return;
    }
    const fingerprint = JSON.stringify(event);
    if (toolsById.get(event.id) === fingerprint) return;
    const existing = toolsById.has(event.id);
    toolsById.set(event.id, fingerprint);
    notify({
      sessionUpdate: existing ? "tool_call_update" : "tool_call",
      toolCallId: event.id,
      title: event.name,
      kind: event.name.includes("terminal")
        ? "execute"
        : event.name.includes("write") || event.name.includes("edit")
          ? "edit"
          : event.name.includes("read")
            ? "read"
            : "other",
      status: event.status === "running" ? "in_progress" : event.status,
      rawInput: event.input,
      ...(event.output === undefined
        ? {}
        : {
            rawOutput: event.output,
            content: [{ type: "content", content: { type: "text", text: event.output } }],
          }),
    });
    offer({
      _tag: "ToolCallUpdated",
      toolCall: {
        toolCallId: event.id,
        title: event.name,
        status: event.status === "running" ? "inProgress" : event.status,
        ...(event.output === undefined ? {} : { detail: event.output }),
        data: {
          toolName: event.name,
          rawInput: event.input,
          ...(event.output === undefined ? {} : { rawOutput: event.output }),
        },
      },
      rawPayload: event,
    });
  };
  const startResult = {
    sessionId: session.sessionId,
    initializeResult: INITIALIZE,
    sessionSetupResult: {
      sessionId: session.sessionId,
      models: {
        currentModelId: "freebuff",
        availableModels: [{ modelId: "freebuff", name: "Freebuff" }],
      },
    },
    modelConfigId: undefined,
  } satisfies AcpSessionRuntime.AcpSessionRuntimeStartResult;

  const drainEvents = Effect.gen(function* () {
    if (closed) return;
    const projected = yield* Deferred.make<void>();
    yield* Queue.offer(notifications, { type: "barrier", acknowledge: projected });
    yield* Deferred.await(projected);
    const acknowledge = yield* Deferred.make<void>();
    yield* Queue.offer(events, { _tag: "EventStreamBarrier", acknowledge });
    yield* Deferred.await(acknowledge);
  });
  const close = Effect.fn("FreebuffRuntime.close")(function* () {
    activePrompt?.abort();
    closed = true;
    yield* Effect.tryPromise({
      try: () => session.close(),
      catch: (cause) =>
        AcpErrors.AcpRequestError.internalError("Freebuff session could not close.", undefined, {
          cause,
        }),
    });
  });
  const runtime: AcpSessionRuntime.AcpSessionRuntime["Service"] = {
    initialize: () => Effect.succeed(INITIALIZE),
    start: () => Effect.succeed(startResult),
    getEvents: () => Stream.fromQueue(events),
    drainEvents,
    getModeState: Effect.succeed(undefined),
    getConfigOptions: Effect.succeed([]),
    loadSession: (sessionId) =>
      sessionId === session.sessionId ? Effect.succeed(startResult) : unsupported("session/load"),
    resumeSession: (sessionId) =>
      sessionId === session.sessionId ? Effect.succeed(startResult) : unsupported("session/resume"),
    forkSession: () => unsupported("session/fork"),
    listSessions: () => unsupported("session/list"),
    closeSession: () => close().pipe(Effect.as({})),
    deleteSession: () => unsupported("session/delete"),
    listProviders: unsupported("provider/list"),
    setProvider: () => unsupported("provider/set"),
    disableProvider: () => unsupported("provider/disable"),
    logout: unsupported("auth/logout"),
    prompt: Effect.fn("FreebuffRuntime.prompt")((payload, promptOptions) =>
      promptLock.withPermits(1)(
        Effect.gen(function* () {
          if (closed)
            return yield* AcpErrors.AcpRequestError.internalError("Freebuff session is closed.");
          const text = payload.prompt
            .map((block) => (block.type === "text" ? block.text : ""))
            .join("\n");
          if (!text.trim())
            return yield* AcpErrors.AcpRequestError.invalidParams(
              "Freebuff requires a text prompt.",
            );
          if (stopPending) {
            stopPending = false;
            if (promptOptions?.dispatched)
              yield* Deferred.succeed(promptOptions.dispatched, undefined);
            return { stopReason: "cancelled" } as const;
          }
          textById.clear();
          completedText.clear();
          toolsById.clear();
          yield* Ref.set(notificationFailure, undefined);
          const controller = new AbortController();
          activePrompt = controller;
          const run = Effect.tryPromise({
            try: (signal) => {
              signal.addEventListener("abort", () => controller.abort(), { once: true });
              const result = session.prompt({ text, signal: controller.signal, onEvent });
              if (promptOptions?.dispatched)
                Deferred.doneUnsafe(promptOptions.dispatched, Effect.void);
              return result;
            },
            catch: (cause) =>
              AcpErrors.AcpRequestError.internalError(
                "Freebuff CLI failed. Check its sign-in and session on this server.",
                undefined,
                { cause },
              ),
          });
          const result = yield* run;
          for (const id of textById.keys()) {
            if (!completedText.has(id)) {
              completedText.add(id);
              offer({ _tag: "AssistantItemCompleted", itemId: id });
            }
          }
          yield* drainEvents;
          const failure = yield* Ref.get(notificationFailure);
          if (failure) return yield* failure;
          return { stopReason: result.cancelled ? "cancelled" : "end_turn" } as const;
        }).pipe(
          Effect.ensuring(
            Effect.sync(() => {
              activePrompt = undefined;
            }),
          ),
        ),
      ),
    ),
    cancel: Effect.sync(() => {
      if (activePrompt) activePrompt.abort();
      else stopPending = true;
    }),
    processContainment: "none",
    terminateProcessGroup: close().pipe(Effect.orDie),
    setMode: () => unsupported("session/set_mode"),
    setSessionModel: (model) =>
      model === "freebuff" ? Effect.succeed({}) : unsupported("session/set_model"),
    setModel: (model) => (model === "freebuff" ? Effect.void : unsupported("session/set_model")),
    setConfigOption: () => unsupported("session/set_config_option"),
    request: (method) => unsupported(method),
    notify: () => Effect.void,
    handleSessionUpdate: (handler) =>
      Effect.sync(() => {
        updateHandlers.push(handler);
      }),
    handleRequestPermission: () => Effect.void,
    handleElicitation: () => Effect.void,
    handleMcpConnect: () => Effect.void,
    handleMcpMessage: () => Effect.void,
    handleMcpDisconnect: () => Effect.void,
    handleMcpNotification: () => Effect.void,
    handleReadTextFile: () => Effect.void,
    handleWriteTextFile: () => Effect.void,
    handleCreateTerminal: () => Effect.void,
    handleTerminalOutput: () => Effect.void,
    handleTerminalWaitForExit: () => Effect.void,
    handleTerminalKill: () => Effect.void,
    handleTerminalRelease: () => Effect.void,
    handleElicitationComplete: () => Effect.void,
    handleUnknownExtRequest: () => Effect.void,
    handleUnknownExtNotification: () => Effect.void,
    handleExtRequest: () => Effect.void,
    handleExtNotification: () => Effect.void,
  };
  return runtime;
});
