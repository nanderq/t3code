import * as NodeAssert from "node:assert/strict";
import { test } from "vite-plus/test";
import { parseReply, requestReply } from "./response-protocol.ts";

const previewOpen = "mcp__t3-code__preview_open";
const previewNavigate = "mcp__t3-code__preview_navigate";
const tools = new Set([previewOpen, previewNavigate]);
const proseReplay =
  'Retrying with the same tool name — the previous load may not have propagated. If it still fails I will fall back to preview_navigate.\n\n{"text":"","toolCalls":[{"name":"mcp__t3-code__preview_open","arguments":{"url":"https://www.google.com","open":true}}]}';
const newlineReplay = '{"text":"Line one\nLine two","toolCalls":[]}';
const validReply = { text: "Done.", toolCalls: [] };
const validResponse = JSON.stringify(validReply);
const toolReply = {
  text: "",
  toolCalls: [{ name: previewOpen, arguments: { url: "https://www.google.com", open: true } }],
};

function protocolError(code, payload) {
  return (error) => {
    NodeAssert.ok(error instanceof Error);
    NodeAssert.ok(!(error instanceof SyntaxError));
    NodeAssert.equal(error.name, "ReplyProtocolError");
    NodeAssert.equal(error.code, code);
    NodeAssert.ok(error.message.length > 0 && error.message.length < 240);
    if (payload) NodeAssert.ok(!error.message.includes(payload));
    NodeAssert.equal(error.cause, undefined);
    return true;
  };
}

function queuedBackend(responses) {
  const requests = [];
  return {
    requests,
    prompt: async (text) => {
      const index = requests.push(text) - 1;
      NodeAssert.ok(index < responses.length, "unexpected extra backend request");
      const response = responses[index];
      if (response instanceof Error) throw response;
      return response;
    },
  };
}

function requestOptions(overrides = {}) {
  return {
    request: "Original external Pi request",
    tools,
    signal: new AbortController().signal,
    ...overrides,
  };
}

test("parses a whole JSON response and an entire labelled JSON fence", () => {
  NodeAssert.deepEqual(parseReply(` \n${JSON.stringify(toolReply)}\t`, tools), toolReply);
  NodeAssert.deepEqual(parseReply(`\n\`\`\`json\n${validResponse}\n\`\`\`\n`, tools), validReply);
  NodeAssert.deepEqual(parseReply(`\`\`\`json\r\n${validResponse}\r\n\`\`\``, tools), validReply);
});

test("preserves Unicode, backticks, braces, and escaped newlines inside valid text", () => {
  const text = '雪 🦊 café — `code` ```json {"nested":"}"} ```\nLine two\\n';
  const reply = {
    text,
    toolCalls: [{ name: previewOpen, arguments: { text, nested: [1, null, true] } }],
  };
  NodeAssert.deepEqual(parseReply(JSON.stringify(reply), tools), reply);
  NodeAssert.deepEqual(parseReply(`\`\`\`json\n${JSON.stringify(reply)}\n\`\`\``, tools), reply);
});

test.each([
  ["the exact prose-prefixed preview_open replay", proseReplay],
  ["the exact physical-newline-in-string replay", newlineReplay],
  ["prose after JSON", `${validResponse}\nAll done.`],
  ["prose around a JSON fence", `Here is the reply:\n\`\`\`json\n${validResponse}\n\`\`\``],
  ["an unlabelled fence", `\`\`\`\n${validResponse}\n\`\`\``],
  ["a JavaScript fence", `\`\`\`javascript\n${validResponse}\n\`\`\``],
  ["multiple JSON objects", `${validResponse}\n${validResponse}`],
  ["multiple fences", `\`\`\`json\n${validResponse}\n\`\`\`\n\`\`\`json\n${validResponse}\n\`\`\``],
  ["JavaScript rather than JSON", '({text:"Done",toolCalls:[]})'],
  ["an empty response", ""],
])("rejects %s without extracting or evaluating a snippet", (_name, response) => {
  NodeAssert.throws(() => parseReply(response, tools), protocolError("INVALID_JSON"));
});

test.each(
  [
    null,
    [],
    1,
    "text",
    {},
    { text: "Done" },
    { toolCalls: [] },
    { text: null, toolCalls: [] },
    { text: "Done", toolCalls: {} },
    { text: "Done", toolCalls: [], extra: true },
  ].map((envelope) => [envelope]),
)("rejects malformed envelope %j", (envelope) => {
  NodeAssert.throws(
    () => parseReply(JSON.stringify(envelope), tools),
    protocolError("INVALID_ENVELOPE"),
  );
});

test.each(
  [
    null,
    [],
    "tool",
    {},
    { name: 1, arguments: {} },
    { name: previewOpen },
    { name: previewOpen, arguments: null },
    { name: previewOpen, arguments: [] },
    { name: previewOpen, arguments: "{}" },
    { name: previewOpen, arguments: 1 },
    { name: previewOpen, arguments: {}, extra: true },
  ].map((call) => [call]),
)("rejects malformed tool call %j", (call) => {
  const response = JSON.stringify({ text: "", toolCalls: [call] });
  NodeAssert.throws(() => parseReply(response, tools), protocolError("INVALID_TOOL_CALL"));
});

test("rejects undeclared tools and does not normalize or register similar names", () => {
  const declared = new Set([previewOpen]);
  for (const name of [previewNavigate, "mcp__t3_code__preview_open", `${previewOpen} `]) {
    NodeAssert.throws(
      () =>
        parseReply(JSON.stringify({ text: "", toolCalls: [{ name, arguments: {} }] }), declared),
      protocolError("UNAVAILABLE_TOOL"),
    );
  }
  NodeAssert.deepEqual([...declared], [previewOpen]);
});

test("allows only exact tool names supplied dynamically by the caller", () => {
  const declared = new Set();
  const name = "mcp__t3-code__preview_newly_loaded";
  const reply = { text: "", toolCalls: [{ name, arguments: {} }] };
  NodeAssert.throws(
    () => parseReply(JSON.stringify(reply), declared),
    protocolError("UNAVAILABLE_TOOL"),
  );
  declared.add(name);
  NodeAssert.deepEqual(parseReply(JSON.stringify(reply), declared), reply);
});

test("toolChoice none rejects calls but accepts text-only responses", () => {
  NodeAssert.throws(
    () => parseReply(JSON.stringify(toolReply), tools, false),
    protocolError("TOOLS_NOT_ALLOWED"),
  );
  NodeAssert.deepEqual(parseReply(validResponse, tools, false), validReply);
  NodeAssert.deepEqual(parseReply(validResponse, new Set()), validReply);
});

test("validation errors omit response text and invalid argument/name payloads", () => {
  const secret = "PRIVATE_RESPONSE_PAYLOAD_abc123";
  NodeAssert.throws(() => parseReply(`${secret} {`, tools), protocolError("INVALID_JSON", secret));
  NodeAssert.throws(
    () => parseReply(JSON.stringify({ text: secret }), tools),
    protocolError("INVALID_ENVELOPE", secret),
  );
  NodeAssert.throws(
    () =>
      parseReply(
        JSON.stringify({ text: secret, toolCalls: [{ name: previewOpen, arguments: secret }] }),
        tools,
      ),
    protocolError("INVALID_TOOL_CALL", secret),
  );
  NodeAssert.throws(
    () =>
      parseReply(JSON.stringify({ text: "", toolCalls: [{ name: secret, arguments: {} }] }), tools),
    protocolError("UNAVAILABLE_TOOL", secret),
  );
});

test("returns a valid initial response without repairs using the supplied callback", async () => {
  const backend = queuedBackend([JSON.stringify(toolReply)]);
  const repairs = [];
  const result = await requestReply(
    requestOptions({ prompt: backend.prompt, onRepair: (info) => repairs.push(info) }),
  );
  NodeAssert.deepEqual(result, { reply: toolReply, repairs: 0 });
  NodeAssert.deepEqual(backend.requests, ["Original external Pi request"]);
  NodeAssert.deepEqual(repairs, []);
});

test.each([
  ["prose-prefixed tool JSON", proseReplay],
  ["physical newlines in strings", newlineReplay],
])(
  "repairs %s through the same backend without emitting extracted tools",
  async (_name, invalid) => {
    const backend = queuedBackend([invalid, JSON.stringify(toolReply)]);
    const completed = [];
    const repairEvents = [];
    const result = await requestReply(
      requestOptions({
        prompt: backend.prompt,
        onRepair: (info) => {
          NodeAssert.deepEqual(completed, []);
          NodeAssert.equal(backend.requests.length, 1);
          repairEvents.push(info);
        },
      }),
    ).then((value) => {
      completed.push(...value.reply.toolCalls);
      return value;
    });
    NodeAssert.deepEqual(result, { reply: toolReply, repairs: 1 });
    NodeAssert.deepEqual(completed, toolReply.toolCalls);
    NodeAssert.equal(repairEvents.length, 1);
    NodeAssert.equal(repairEvents[0].attempt, 1);
    NodeAssert.ok(repairEvents[0].reason.length > 0);
    NodeAssert.ok(!repairEvents[0].reason.includes(invalid));
    NodeAssert.equal(backend.requests.length, 2);
    const correction = backend.requests[1];
    NodeAssert.match(correction, /exactly one JSON object/i);
    NodeAssert.match(correction, /escape newlines/i);
    NodeAssert.ok(correction.includes("\\n"));
    NodeAssert.match(correction, /do not invoke native tools/i);
    NodeAssert.match(correction, /use only (?:the )?current external tool names/i);
    NodeAssert.match(correction, /do not repeat (?:any )?earlier completed tools/i);
    for (const tool of tools) NodeAssert.ok(correction.includes(tool));
    NodeAssert.ok(!correction.includes(invalid));
  },
);

test("permits two corrective prompts and returns the second corrected response", async () => {
  const backend = queuedBackend([newlineReplay, '{"text":false,"toolCalls":[]}', validResponse]);
  const attempts = [];
  const result = await requestReply(
    requestOptions({ prompt: backend.prompt, onRepair: ({ attempt }) => attempts.push(attempt) }),
  );
  NodeAssert.deepEqual(result, { reply: validReply, repairs: 2 });
  NodeAssert.deepEqual(attempts, [1, 2]);
  NodeAssert.equal(backend.requests.length, 3);
});

test("permanently invalid output exhausts exactly two repairs with a bounded protocol error", async () => {
  const secret = "PRIVATE_PERMANENT_INVALID_PAYLOAD";
  const backend = queuedBackend([secret, secret, secret]);
  const events = [];
  await NodeAssert.rejects(
    requestReply(requestOptions({ prompt: backend.prompt, onRepair: (info) => events.push(info) })),
    (error) => {
      protocolError("REPAIR_EXHAUSTED", secret)(error);
      NodeAssert.match(error.message, /2 corrective prompts/i);
      return true;
    },
  );
  NodeAssert.equal(backend.requests.length, 3);
  NodeAssert.deepEqual(
    events.map(({ attempt }) => attempt),
    [1, 2],
  );
  NodeAssert.ok(events.every(({ reason }) => !reason.includes(secret)));
});

test("corrects an unavailable tool to a currently declared tool", async () => {
  const unknownName = "PRIVATE_UNAVAILABLE_TOOL";
  const invalid = JSON.stringify({ text: "", toolCalls: [{ name: unknownName, arguments: {} }] });
  const backend = queuedBackend([invalid, JSON.stringify(toolReply)]);
  const result = await requestReply(requestOptions({ prompt: backend.prompt }));
  NodeAssert.deepEqual(result, { reply: toolReply, repairs: 1 });
  NodeAssert.ok(backend.requests[1].includes(previewOpen));
  NodeAssert.ok(!backend.requests[1].includes(unknownName));
});

test("validates the entire batch before returning any tool calls", async () => {
  const invalid = JSON.stringify({
    text: "",
    toolCalls: [...toolReply.toolCalls, { name: "unavailable", arguments: {} }],
  });
  NodeAssert.throws(() => parseReply(invalid, tools), protocolError("UNAVAILABLE_TOOL"));
  const emitted = [];
  const backend = queuedBackend([invalid, validResponse]);
  const result = await requestReply(
    requestOptions({
      prompt: backend.prompt,
      onRepair: () => NodeAssert.deepEqual(emitted, []),
    }),
  ).then((value) => {
    emitted.push(...value.reply.toolCalls);
    return value;
  });
  NodeAssert.deepEqual(result, { reply: validReply, repairs: 1 });
  NodeAssert.deepEqual(emitted, []);
});

test("repair instructions and validation use tool names added to the supplied Set", async () => {
  const declared = new Set([previewOpen]);
  const name = "mcp__t3-code__preview_added_during_repair";
  const reply = { text: "", toolCalls: [{ name, arguments: {} }] };
  const backend = queuedBackend([newlineReplay, JSON.stringify(reply)]);
  const result = await requestReply(
    requestOptions({
      tools: declared,
      prompt: backend.prompt,
      onRepair: () => declared.add(name),
    }),
  );
  NodeAssert.deepEqual(result, { reply, repairs: 1 });
  NodeAssert.ok(backend.requests[1].includes(previewOpen));
  NodeAssert.ok(backend.requests[1].includes(name));
});

test("repairs a tool request under toolChoice none to a text-only response", async () => {
  const backend = queuedBackend([JSON.stringify(toolReply), validResponse]);
  const result = await requestReply(requestOptions({ allowTools: false, prompt: backend.prompt }));
  NodeAssert.deepEqual(result, { reply: validReply, repairs: 1 });
  NodeAssert.match(backend.requests[1], /toolChoice is none/i);
  NodeAssert.match(backend.requests[1], /"toolCalls":\s*\[\]/);
  NodeAssert.match(backend.requests[1], /allowed external tool names:\s*\[\]/i);
});

test.each([
  ["native process error", new Error("Native process exited")],
  ["backend SyntaxError", new SyntaxError("Backend failed parsing its own data")],
  [
    "backend error with a protocol-shaped code",
    Object.assign(new Error("Backend failure"), { code: "INVALID_JSON" }),
  ],
])("%s passes through unchanged without retry", async (_name, backendError) => {
  const backend = queuedBackend([backendError]);
  const events = [];
  await NodeAssert.rejects(
    requestReply(requestOptions({ prompt: backend.prompt, onRepair: (info) => events.push(info) })),
    (error) => error === backendError,
  );
  NodeAssert.equal(backend.requests.length, 1);
  NodeAssert.deepEqual(events, []);
});

test("a backend error during a corrective prompt is not retried", async () => {
  const backendError = new Error("Native process unavailable during repair");
  const backend = queuedBackend([newlineReplay, backendError]);
  const attempts = [];
  await NodeAssert.rejects(
    requestReply(
      requestOptions({ prompt: backend.prompt, onRepair: ({ attempt }) => attempts.push(attempt) }),
    ),
    (error) => error === backendError,
  );
  NodeAssert.equal(backend.requests.length, 2);
  NodeAssert.deepEqual(attempts, [1]);
});

test("a rejected onRepair callback passes through without another backend request", async () => {
  const callbackError = new Error("Repair observer failed");
  const backend = queuedBackend([newlineReplay]);
  await NodeAssert.rejects(
    requestReply(
      requestOptions({
        prompt: backend.prompt,
        onRepair: async () => {
          throw callbackError;
        },
      }),
    ),
    (error) => error === callbackError,
  );
  NodeAssert.equal(backend.requests.length, 1);
});

test("an already aborted request never calls the backend or onRepair", async () => {
  const controller = new AbortController();
  const reason = new Error("Stopped before request");
  controller.abort(reason);
  const backend = queuedBackend([]);
  const events = [];
  await NodeAssert.rejects(
    requestReply(
      requestOptions({
        signal: controller.signal,
        prompt: backend.prompt,
        onRepair: (info) => events.push(info),
      }),
    ),
    (error) => error === reason,
  );
  NodeAssert.deepEqual(backend.requests, []);
  NodeAssert.deepEqual(events, []);
});

test.each([
  ["valid", validResponse],
  ["invalid", newlineReplay],
])(
  "abort immediately after a %s response prevents returning or repairing it",
  async (_name, response) => {
    const controller = new AbortController();
    const reason = new Error("Stopped after response");
    const requests = [];
    const events = [];
    await NodeAssert.rejects(
      requestReply(
        requestOptions({
          signal: controller.signal,
          prompt: async (text) => {
            requests.push(text);
            controller.abort(reason);
            return response;
          },
          onRepair: (info) => events.push(info),
        }),
      ),
      (error) => error === reason,
    );
    NodeAssert.equal(requests.length, 1);
    NodeAssert.deepEqual(events, []);
  },
);

test("an abort event during the initial awaited response prevents repair", async () => {
  const controller = new AbortController();
  const reason = new Error("Stopped during response");
  let requests = 0;
  const pending = requestReply(
    requestOptions({
      signal: controller.signal,
      prompt: () => {
        requests += 1;
        return new Promise((resolve) => {
          controller.signal.addEventListener("abort", () => resolve(newlineReplay), { once: true });
        });
      },
    }),
  );
  controller.abort(reason);
  await NodeAssert.rejects(pending, (error) => error === reason);
  NodeAssert.equal(requests, 1);
});

test("an abort event during a corrective response prevents another repair request", async () => {
  const controller = new AbortController();
  const reason = new Error("Stopped during corrective response");
  const started = Promise.withResolvers();
  const requests = [];
  const attempts = [];
  const pending = requestReply(
    requestOptions({
      signal: controller.signal,
      prompt: async (text) => {
        requests.push(text);
        if (requests.length === 1) return newlineReplay;
        return new Promise((resolve) => {
          controller.signal.addEventListener("abort", () => resolve(newlineReplay), { once: true });
          started.resolve();
        });
      },
      onRepair: ({ attempt }) => attempts.push(attempt),
    }),
  );
  await started.promise;
  controller.abort(reason);
  await NodeAssert.rejects(pending, (error) => error === reason);
  NodeAssert.equal(requests.length, 2);
  NodeAssert.deepEqual(attempts, [1]);
});

test("abort inside synchronous onRepair prevents a corrective backend request", async () => {
  const controller = new AbortController();
  const reason = new Error("Stopped by repair observer");
  const backend = queuedBackend([newlineReplay]);
  await NodeAssert.rejects(
    requestReply(
      requestOptions({
        signal: controller.signal,
        prompt: backend.prompt,
        onRepair: () => controller.abort(reason),
      }),
    ),
    (error) => error === reason,
  );
  NodeAssert.equal(backend.requests.length, 1);
});

test("abort during awaited onRepair prevents a corrective backend request", async () => {
  const controller = new AbortController();
  const reason = new Error("Stopped during asynchronous repair observer");
  const started = Promise.withResolvers();
  const backend = queuedBackend([newlineReplay]);
  const pending = requestReply(
    requestOptions({
      signal: controller.signal,
      prompt: backend.prompt,
      onRepair: () =>
        new Promise((resolve) => {
          controller.signal.addEventListener("abort", () => resolve(), { once: true });
          started.resolve();
        }),
    }),
  );
  await started.promise;
  controller.abort(reason);
  await NodeAssert.rejects(pending, (error) => error === reason);
  NodeAssert.equal(backend.requests.length, 1);
});
