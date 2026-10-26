export type ReplyProtocolErrorCode =
  | "INVALID_JSON"
  | "INVALID_ENVELOPE"
  | "INVALID_TOOL_CALL"
  | "UNAVAILABLE_TOOL"
  | "TOOLS_NOT_ALLOWED"
  | "REPAIR_EXHAUSTED";

class ReplyProtocolError extends Error {
  readonly code: ReplyProtocolErrorCode;

  constructor(code: ReplyProtocolErrorCode, message: string) {
    super(message);
    this.name = "ReplyProtocolError";
    this.code = code;
  }
}

function record(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function parseReply(text: string, tools: ReadonlySet<string>, allowTools = true) {
  const cleaned = text.trim();
  const fence = /^```json[\t ]*\r?\n([\s\S]*?)\r?\n```$/.exec(cleaned);
  let reply: unknown;
  try {
    reply = JSON.parse(fence ? fence[1]! : cleaned);
  } catch (error) {
    if (!(error instanceof SyntaxError)) throw error;
    // JSON.parse messages can include private response text, so do not retain the cause.
    throw new ReplyProtocolError(
      "INVALID_JSON",
      "Response must be one complete valid JSON object.",
    );
  }
  if (
    !record(reply) ||
    Object.keys(reply).length !== 2 ||
    typeof reply.text !== "string" ||
    !Array.isArray(reply.toolCalls)
  ) {
    throw new ReplyProtocolError(
      "INVALID_ENVELOPE",
      "Response must contain only text (string) and toolCalls (array).",
    );
  }
  if (!allowTools && reply.toolCalls.length > 0) {
    throw new ReplyProtocolError(
      "TOOLS_NOT_ALLOWED",
      "Tool calls are disabled because toolChoice is none.",
    );
  }
  const toolCalls = reply.toolCalls.map((value: unknown) => {
    if (
      !record(value) ||
      Object.keys(value).length !== 2 ||
      typeof value.name !== "string" ||
      !record(value.arguments)
    ) {
      throw new ReplyProtocolError(
        "INVALID_TOOL_CALL",
        "Each tool call must contain only name (string) and arguments (object).",
      );
    }
    if (!tools.has(value.name)) {
      throw new ReplyProtocolError(
        "UNAVAILABLE_TOOL",
        "Response requested an undeclared external tool.",
      );
    }
    return { name: value.name, arguments: value.arguments };
  });
  return { text: reply.text, toolCalls };
}

function correction(tools: ReadonlySet<string>, allowTools: boolean, code: ReplyProtocolErrorCode) {
  return `Your previous response violated the external Pi response protocol (${code}).
Output exactly one JSON object with no prose or Markdown:
{"text":"assistant text, or empty string","toolCalls":[{"name":"declared_tool_name","arguments":{}}]}
Escape newlines inside strings as \\n; never put physical newlines inside JSON strings.
Do not invoke native tools, agents, commands, or filesystem operations.
Use only current external tool names. Allowed external tool names: ${JSON.stringify(allowTools ? [...tools] : [])}.
${allowTools ? 'If no external tools are needed, return "toolCalls": [].' : 'toolChoice is none: return "toolCalls": [] and answer in text.'}
Do not repeat earlier completed tools; use their existing results from the conversation.
Return the complete corrected response to the original request.`;
}

export async function requestReply(options: {
  request: string;
  tools: ReadonlySet<string>;
  signal: AbortSignal;
  allowTools?: boolean;
  prompt: (text: string) => Promise<string>;
  onRepair?: (info: { attempt: number; reason: string }) => void | Promise<void>;
}): Promise<{ reply: ReturnType<typeof parseReply>; repairs: number }> {
  const { request, tools, signal, allowTools = true, prompt, onRepair } = options;
  let nextRequest = request;
  let repairs = 0;
  while (true) {
    signal.throwIfAborted();
    let response: string;
    try {
      response = await prompt(nextRequest);
    } finally {
      signal.throwIfAborted();
    }

    // Only parser failures are repairable; backend and observer errors must pass through.
    let failure: ReplyProtocolError;
    try {
      return { reply: parseReply(response, tools, allowTools), repairs };
    } catch (error) {
      if (!(error instanceof ReplyProtocolError)) throw error;
      failure = error;
    }
    if (repairs === 2) {
      throw new ReplyProtocolError(
        "REPAIR_EXHAUSTED",
        `Response protocol remained invalid after 2 corrective prompts (${failure.code}).`,
      );
    }
    repairs += 1;
    if (onRepair) {
      signal.throwIfAborted();
      try {
        await onRepair({ attempt: repairs, reason: failure.message });
      } finally {
        signal.throwIfAborted();
      }
    }
    nextRequest = correction(tools, allowTools, failure.code);
  }
}
