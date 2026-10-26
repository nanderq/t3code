import * as NodeAssert from "node:assert/strict";
import * as NodeCrypto from "node:crypto";
import * as NodeFSP from "node:fs/promises";
import * as NodeOS from "node:os";
import * as NodePath from "node:path";
import { test } from "vite-plus/test";
import { Agent } from "@earendil-works/pi-agent-core";
import { Type, normalizeContext } from "@earendil-works/pi-ai";
import { makeFreebuffBridge } from "./freebuff-provider.ts";

const model = {
  id: "freebuff",
  name: "Test Freebuff",
  provider: "freebuff-pi",
  api: "freebuff-cli-experiment",
  baseUrl: "cli://freebuff",
  reasoning: false,
  input: ["text"],
  contextWindow: 32_000,
  maxTokens: 8192,
  cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
};

async function fixture(responses) {
  const directory = await NodeFSP.mkdtemp(
    NodePath.join(NodeOS.tmpdir(), "freebuff-pi-regression-"),
  );
  const requests = [];
  let sessions = 0;
  const bridge = makeFreebuffBridge({
    directory,
    async createSession() {
      sessions++;
      return {
        sessionId: "fake-native-session",
        async prompt(input) {
          requests.push(input.text);
          const next = responses.shift();
          NodeAssert.notEqual(next, undefined, "Unexpected additional native prompt");
          const text = typeof next === "function" ? next(input) : next;
          input.onEvent({ type: "text", id: "reply-" + requests.length, text, complete: true });
          return { cancelled: false };
        },
        async close() {},
      };
    },
  });
  return {
    ...bridge,
    requests,
    get sessions() {
      return sessions;
    },
    async close() {
      await bridge.closeNativeSession();
      await NodeFSP.rm(directory, { recursive: true, force: true });
    },
  };
}

test("real Pi executes a deferred tool exactly once after repairing the observed prose-prefixed response", async () => {
  let toolCalls = 0;
  const receipt = NodeCrypto.randomUUID();
  const toolName = "mcp__t3-code__preview_open";
  const call = JSON.stringify({
    text: "",
    toolCalls: [{ name: toolName, arguments: { url: "https://www.google.com", open: true } }],
  });
  const prefix =
    "Retrying with the same tool name — the previous load may not have propagated. If it still fails I'll fall back to `preview_navigate`.\n\n";
  const f = await fixture([
    JSON.stringify({ text: "", toolCalls: [{ name: "tool_search", arguments: {} }] }),
    prefix + call,
    call,
    JSON.stringify({ text: receipt, toolCalls: [] }),
  ]);
  const tools = [
    {
      name: "tool_search",
      label: "Fake local tool search",
      description: "Loads the fake local deferred tool. No real MCP or browser calls.",
      parameters: Type.Object({}),
      async execute() {
        agent.state.tools = [
          ...tools,
          {
            name: toolName,
            label: "Fake local preview tool",
            description: "Returns a receipt without opening a browser.",
            parameters: Type.Object({ url: Type.String(), open: Type.Boolean() }),
            async execute() {
              toolCalls++;
              return { content: [{ type: "text", text: receipt }], details: {} };
            },
          },
        ];
        return { content: [{ type: "text", text: "Loaded " + toolName }], details: {} };
      },
    },
  ];
  const agent = new Agent({
    initialState: { model, thinkingLevel: "off", tools },
    streamFn: f.streamFreebuff,
    prepareNextTurnWithContext: ({ context }) => ({
      context: { ...context, tools: agent.state.tools },
    }),
  });
  try {
    await agent.prompt("Exercise the fake local deferred tool.");
    NodeAssert.equal(toolCalls, 1);
    NodeAssert.equal(f.requests.length, 4);
    NodeAssert.equal(f.sessions, 1, "Format repair must reuse the same native session");
    NodeAssert.ok(f.requests[1].includes(toolName), "Dynamically added Pi tool reaches Freebuff");
    NodeAssert.equal(agent.state.messages.at(-1).stopReason, "stop");
    NodeAssert.equal(agent.state.messages.at(-1).content[0].text, receipt);
    NodeAssert.equal(
      agent.state.messages.filter(
        (message) => message.role === "assistant" && message.stopReason === "error",
      ).length,
      0,
    );
  } finally {
    await f.close();
  }
});

test("real Pi receives valid multiline text after the observed unescaped-newline JSON defect", async () => {
  const f = await fixture([
    '{"text":"Line one\nLine two","toolCalls":[]}',
    JSON.stringify({ text: "Line one\nLine two", toolCalls: [] }),
  ]);
  const agent = new Agent({
    initialState: { model, thinkingLevel: "off", tools: [] },
    streamFn: f.streamFreebuff,
  });
  try {
    await agent.prompt("Return multiline text.");
    NodeAssert.equal(agent.state.messages.at(-1).stopReason, "stop");
    NodeAssert.equal(agent.state.messages.at(-1).content[0].text, "Line one\nLine two");
    NodeAssert.equal(f.requests.length, 2);
    NodeAssert.equal(f.sessions, 1);
  } finally {
    await f.close();
  }
});

test("native tool execution aborts without protocol repair or Pi tool replay", async () => {
  const f = await fixture([
    (input) => {
      input.onEvent({
        type: "tool",
        id: "native-call",
        name: "read_files",
        input: {},
        status: "running",
      });
      return '{"text":"","toolCalls":[]}';
    },
  ]);
  const agent = new Agent({
    initialState: { model, thinkingLevel: "off", tools: [] },
    streamFn: f.streamFreebuff,
  });
  try {
    await agent.prompt("Do not execute native tools.");
    NodeAssert.equal(f.requests.length, 1);
    NodeAssert.equal(agent.state.messages.at(-1).stopReason, "error");
    NodeAssert.match(agent.state.messages.at(-1).errorMessage, /Native Freebuff tool execution/);
    NodeAssert.equal(
      agent.state.messages.filter((message) => message.role === "toolResult").length,
      0,
    );
  } finally {
    await f.close();
  }
});

test("aborting from a streamed tool event ends aborted and never executes the Pi tool", async () => {
  let toolCalls = 0;
  const f = await fixture([
    JSON.stringify({ text: "", toolCalls: [{ name: "pi_stop_probe", arguments: {} }] }),
  ]);
  const agent = new Agent({
    initialState: {
      model,
      thinkingLevel: "off",
      tools: [
        {
          name: "pi_stop_probe",
          label: "Stop probe",
          description: "Must never execute after Stop.",
          parameters: Type.Object({}),
          async execute() {
            toolCalls++;
            return { content: [{ type: "text", text: "should not execute" }], details: {} };
          },
        },
      ],
    },
    streamFn: f.streamFreebuff,
  });
  agent.subscribe((event) => {
    if (event.type === "message_update" && event.assistantMessageEvent.type === "toolcall_end")
      agent.abort();
  });
  try {
    await agent.prompt("Request the stop probe.");
    NodeAssert.equal(agent.state.messages.at(-1).stopReason, "aborted");
    NodeAssert.equal(toolCalls, 0);
    NodeAssert.equal(f.requests.length, 1);
    NodeAssert.equal(agent.state.isStreaming, false);
  } finally {
    await f.close();
  }
});

test("the raw provider stream cannot emit done after cancellation from a partial event", async () => {
  const f = await fixture([
    JSON.stringify({ text: "", toolCalls: [{ name: "pi_stop_probe", arguments: {} }] }),
  ]);
  const controller = new AbortController();
  const context = normalizeContext({
    messages: [{ role: "user", content: "Request the stop probe.", timestamp: 0 }],
    tools: [{ name: "pi_stop_probe", description: "Stop probe", parameters: Type.Object({}) }],
  });
  const stream = f.streamFreebuff(model, context, { signal: controller.signal });
  const terminal = [];
  try {
    for await (const event of stream) {
      if (event.type === "toolcall_end") controller.abort();
      if (event.type === "done" || event.type === "error") terminal.push(event);
    }
    NodeAssert.equal((await stream.result()).stopReason, "aborted");
    NodeAssert.deepEqual(
      terminal.map((event) => event.type),
      ["error"],
    );
    NodeAssert.equal(f.requests.length, 1);
  } finally {
    await f.close();
  }
});
