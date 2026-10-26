export type FreebuffCliEvent =
  | {
      readonly type: "text";
      readonly id: string;
      readonly text: string;
      readonly complete: boolean;
    }
  | {
      readonly type: "tool";
      readonly id: string;
      readonly name: string;
      readonly input: unknown;
      readonly output?: string;
      readonly status: "pending" | "running" | "completed" | "failed";
    };

export interface FreebuffMessage {
  readonly id: string;
  readonly variant: string;
  readonly content: string;
  readonly blocks: readonly unknown[];
  readonly complete: boolean;
  readonly error?: string;
}

export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function decodeFreebuffMessages(value: unknown): readonly FreebuffMessage[] {
  if (!Array.isArray(value)) throw new Error("Invalid Freebuff transcript.");
  return value.map((message: unknown) => {
    if (
      !isRecord(message) ||
      typeof message.id !== "string" ||
      typeof message.variant !== "string"
    ) {
      throw new Error("Invalid Freebuff transcript message.");
    }
    return {
      id: message.id,
      variant: message.variant,
      content: typeof message.content === "string" ? message.content : "",
      blocks: Array.isArray(message.blocks) ? message.blocks : [],
      complete: message.isComplete === true,
      ...(typeof message.userError === "string" ? { error: message.userError } : {}),
    };
  });
}

function hasToolError(value: unknown): boolean {
  if (Array.isArray(value)) return value.some(hasToolError);
  return (
    isRecord(value) &&
    (typeof value.errorMessage === "string" ||
      value.isError === true ||
      (isRecord(value.value) && hasToolError(value.value)))
  );
}

export function projectFreebuffEvents(messages: readonly FreebuffMessage[]): FreebuffCliEvent[] {
  const events: FreebuffCliEvent[] = [];
  for (const message of messages) {
    if (message.variant !== "ai" && message.variant !== "agent") continue;
    const text =
      message.content ||
      message.blocks
        .filter(
          (block): block is Record<string, unknown> =>
            isRecord(block) && block.type === "text" && block.textType !== "reasoning",
        )
        .map((block) => (typeof block.content === "string" ? block.content : ""))
        .join("");
    if (text) events.push({ type: "text", id: message.id, text, complete: message.complete });

    const visit = (blocks: readonly unknown[]) => {
      for (const block of blocks) {
        if (!isRecord(block)) continue;
        if (block.type === "agent" && Array.isArray(block.blocks)) visit(block.blocks);
        if (
          block.type !== "tool" ||
          typeof block.toolCallId !== "string" ||
          typeof block.toolName !== "string"
        ) {
          continue;
        }
        const output = typeof block.output === "string" ? block.output : undefined;
        events.push({
          type: "tool",
          id: `${message.id}:${block.toolCallId}`,
          name: block.toolName,
          input: block.input,
          ...(output !== undefined ? { output } : {}),
          status: hasToolError(block.outputRaw)
            ? "failed"
            : output !== undefined || block.outputRaw !== undefined
              ? "completed"
              : "running",
        });
      }
    };
    visit(message.blocks);
  }
  return events;
}

export function readFreebuffLogLines(text: string) {
  const lines = text.split("\n");
  const remainder = lines.pop() ?? "";
  const entries: Record<string, unknown>[] = [];
  for (const line of lines) {
    try {
      const value: unknown = JSON.parse(line);
      if (isRecord(value)) entries.push(value);
    } catch {
      // A diagnostic logger is not a transport: unrelated malformed records cannot end a turn.
    }
  }
  return { entries, remainder };
}
