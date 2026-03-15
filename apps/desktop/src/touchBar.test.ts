import { ProjectId } from "@t3tools/contracts";
import { beforeEach, describe, expect, it, vi } from "vitest";

const touchBarMocks = vi.hoisted(() => {
  class MockTouchBar {
    items: unknown[];

    constructor({ items }: { items: unknown[] }) {
      this.items = items;
    }
  }

  class MockTouchBarButton {
    label: string;
    enabled: boolean | undefined;
    click: () => void;

    constructor({
      label,
      enabled,
      click,
    }: {
      label: string;
      enabled?: boolean;
      click: () => void;
    }) {
      this.label = label;
      this.enabled = enabled;
      this.click = click;
    }
  }

  class MockTouchBarPopover {
    label: string;
    showCloseButton: boolean | undefined;
    items: MockTouchBar;

    constructor({
      label,
      showCloseButton,
      items,
    }: {
      label: string;
      showCloseButton?: boolean;
      items: MockTouchBar;
    }) {
      this.label = label;
      this.showCloseButton = showCloseButton;
      this.items = items;
    }
  }

  class MockTouchBarGroup {
    items: MockTouchBar;

    constructor({ items }: { items: MockTouchBar }) {
      this.items = items;
    }
  }

  class MockTouchBarSpacer {
    size: string;

    constructor({ size }: { size: string }) {
      this.size = size;
    }
  }

  Object.assign(MockTouchBar, {
    TouchBarButton: MockTouchBarButton,
    TouchBarPopover: MockTouchBarPopover,
    TouchBarGroup: MockTouchBarGroup,
    TouchBarSpacer: MockTouchBarSpacer,
  });

  return {
    MockTouchBar,
    MockTouchBarButton,
    MockTouchBarPopover,
  };
});

vi.mock("electron", () => ({
  TouchBar: touchBarMocks.MockTouchBar,
}));

import { buildTouchBar } from "./touchBar";

describe("buildTouchBar", () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  it("renders a project popover with the provided label", () => {
    const touchBar = buildTouchBar(
      {
        project: {
          label: "First",
          items: [{ id: ProjectId.makeUnsafe("project-1"), label: "First" }],
        },
        editor: null,
        git: null,
      },
      vi.fn(),
    ) as unknown as { items: unknown[] };

    expect(touchBar.items[0]).toBeInstanceOf(touchBarMocks.MockTouchBarPopover);
    expect(touchBar.items[0]).toMatchObject({ label: "First", showCloseButton: true });
  });

  it("emits project.select for popover buttons", () => {
    const onAction = vi.fn();
    const touchBar = buildTouchBar(
      {
        project: {
          label: "Select Project",
          items: [
            { id: ProjectId.makeUnsafe("project-1"), label: "First" },
            { id: ProjectId.makeUnsafe("project-2"), label: "Second" },
          ],
        },
        editor: null,
        git: null,
      },
      onAction,
    ) as unknown as {
      items: Array<{
        items?: {
          items: Array<{
            click: () => void;
          }>;
        };
      }>;
    };

    touchBar.items[0]?.items?.items[1]?.click();

    expect(onAction).toHaveBeenCalledWith({
      type: "project.select",
      projectId: "project-2",
    });
  });

  it("renders commit and push buttons when git state is present", () => {
    const touchBar = buildTouchBar(
      {
        project: null,
        editor: null,
        git: {
          commitEnabled: true,
          pushEnabled: false,
        },
      },
      vi.fn(),
    ) as unknown as {
      items: Array<{
        items?: {
          items: Array<{
            label: string;
            enabled?: boolean;
          }>;
        };
      }>;
    };

    expect(touchBar.items[0]?.items?.items).toMatchObject([
      { label: "Commit", enabled: true },
      { label: "Push", enabled: false },
    ]);
  });

  it("uses a flexible spacer to push git actions to the right of the touch bar", () => {
    const touchBar = buildTouchBar(
      {
        project: {
          label: "Project",
          items: [{ id: ProjectId.makeUnsafe("project-1"), label: "Project" }],
        },
        editor: {
          label: "Open",
          enabled: true,
        },
        git: {
          commitEnabled: true,
          pushEnabled: true,
        },
      },
      vi.fn(),
    ) as unknown as {
      items: Array<{
        size?: string;
      }>;
    };

    expect(touchBar.items[3]).toMatchObject({ size: "flexible" });
  });

  it("emits git actions for commit and push buttons", () => {
    const onAction = vi.fn();
    const touchBar = buildTouchBar(
      {
        project: null,
        editor: null,
        git: {
          commitEnabled: true,
          pushEnabled: true,
        },
      },
      onAction,
    ) as unknown as {
      items: Array<{
        items?: {
          items: Array<{
            click: () => void;
          }>;
        };
      }>;
    };

    touchBar.items[0]?.items?.items[0]?.click();
    touchBar.items[0]?.items?.items[1]?.click();

    expect(onAction).toHaveBeenNthCalledWith(1, { type: "git.commit" });
    expect(onAction).toHaveBeenNthCalledWith(2, { type: "git.push" });
  });
});
