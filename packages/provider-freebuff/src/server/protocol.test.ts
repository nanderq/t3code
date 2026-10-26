import { describe, expect, it } from "@effect/vitest";

import { decodeFreebuffMessages, projectFreebuffEvents, readFreebuffLogLines } from "./protocol.ts";

describe("Freebuff structured transcript", () => {
  it("projects assistant text and nested tools without forwarding internal run state", () => {
    const messages = decodeFreebuffMessages([
      { id: "user", variant: "user", content: "Hello" },
      {
        id: "assistant",
        variant: "ai",
        content: "",
        isComplete: true,
        metadata: { runState: { private: "do not forward" } },
        blocks: [
          { type: "text", content: "Thinking", textType: "reasoning" },
          { type: "text", content: "Hello" },
          {
            type: "agent",
            agentId: "worker",
            blocks: [
              { type: "tool", toolCallId: "read", toolName: "read_files", input: { path: "a" } },
            ],
          },
          {
            type: "tool",
            toolCallId: "write",
            toolName: "write_file",
            input: { path: "b" },
            output: "Saved",
          },
        ],
      },
    ]);
    const events = projectFreebuffEvents(messages);
    expect(events).toEqual([
      { type: "text", id: "assistant", text: "Hello", complete: true },
      {
        type: "tool",
        id: "assistant:read",
        name: "read_files",
        input: { path: "a" },
        status: "running",
      },
      {
        type: "tool",
        id: "assistant:write",
        name: "write_file",
        input: { path: "b" },
        output: "Saved",
        status: "completed",
      },
    ]);
    expect(JSON.stringify(events)).not.toContain("do not forward");
  });

  it("preserves full text snapshots and detects structured tool failures", () => {
    const events = projectFreebuffEvents(
      decodeFreebuffMessages([
        {
          id: "assistant",
          variant: "ai",
          content: "Complete answer",
          isComplete: false,
          blocks: [
            { type: "text", content: "Duplicate answer" },
            {
              type: "tool",
              toolCallId: "command",
              toolName: "run_terminal_command",
              input: {},
              output: "Failed",
              outputRaw: [{ type: "json", value: { errorMessage: "Command failed" } }],
            },
          ],
        },
      ]),
    );
    expect(events).toMatchObject([
      { type: "text", text: "Complete answer", complete: false },
      { type: "tool", status: "failed" },
    ]);
  });

  it("rejects invalid documents and ignores unsupported blocks", () => {
    expect(() => decodeFreebuffMessages(null)).toThrow("Invalid Freebuff transcript");
    expect(() => decodeFreebuffMessages([{ id: 1, variant: "ai" }])).toThrow();
    expect(
      projectFreebuffEvents(
        decodeFreebuffMessages([
          { id: "divider", variant: "ai", blocks: [{ type: "mode-divider", mode: "LITE" }] },
        ]),
      ),
    ).toEqual([]);
  });

  it("buffers split JSONL records and recognizes only native root completion", () => {
    const first = readFreebuffLogLines('{"msg":"unrelated","data":{}}\n{"msg":"Main prompt');
    expect(first.entries).toEqual([{ msg: "unrelated", data: {} }]);
    expect(first.remainder).toBe('{"msg":"Main prompt');
    const second = readFreebuffLogLines(
      first.remainder + ' finished","data":{"outputType":"lastMessage"}}\ninvalid\n',
    );
    expect(second.entries).toEqual([
      { msg: "Main prompt finished", data: { outputType: "lastMessage" } },
    ]);
    expect(second.remainder).toBe("");
  });
});
