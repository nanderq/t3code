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
    icon: unknown;
    iconPosition: string | undefined;
    click: () => void;

    constructor({
      label,
      enabled,
      icon,
      iconPosition,
      click,
    }: {
      label: string;
      enabled?: boolean;
      icon?: unknown;
      iconPosition?: string;
      click: () => void;
    }) {
      this.label = label;
      this.enabled = enabled;
      this.icon = icon;
      this.iconPosition = iconPosition;
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

  class MockTouchBarScrubber {
    items: Array<{
      label?: string;
      icon?: unknown;
    }>;
    mode: string | undefined;
    showArrowButtons: boolean | undefined;
    highlight: ((highlightedIndex: number) => void) | undefined;

    constructor({
      items,
      mode,
      showArrowButtons,
      highlight,
    }: {
      items: Array<{
        label?: string;
        icon?: unknown;
      }>;
      mode?: string;
      showArrowButtons?: boolean;
      highlight?: (highlightedIndex: number) => void;
    }) {
      this.items = items;
      this.mode = mode;
      this.showArrowButtons = showArrowButtons;
      this.highlight = highlight;
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
    TouchBarScrubber: MockTouchBarScrubber,
    TouchBarGroup: MockTouchBarGroup,
    TouchBarSpacer: MockTouchBarSpacer,
  });

  return {
    MockTouchBar,
    MockTouchBarButton,
    MockTouchBarPopover,
    MockTouchBarScrubber,
    nativeImage: {
      createFromPath: vi.fn(() => ({
        resize: vi.fn(() => ({ kind: "native-image" })),
      })),
    },
  };
});

vi.mock("electron", () => ({
  TouchBar: touchBarMocks.MockTouchBar,
  nativeImage: touchBarMocks.nativeImage,
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
        activeProjectId: null,
        editor: null,
        git: null,
      },
      vi.fn(),
    ) as unknown as {
      items: Array<{
        items?: {
          items: unknown[];
        };
      }>;
    };

    expect(touchBar.items[0]).toBeInstanceOf(touchBarMocks.MockTouchBarPopover);
    expect(touchBar.items[0]).toMatchObject({ label: "First", showCloseButton: true });
    expect(touchBarMocks.nativeImage.createFromPath).toHaveBeenCalled();
    expect(touchBar.items[0]?.items?.items[0]).toBeInstanceOf(touchBarMocks.MockTouchBarScrubber);
  });

  it("emits project.select for popover scrubber taps", () => {
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
        activeProjectId: null,
        editor: null,
        git: null,
      },
      onAction,
    ) as unknown as {
      items: Array<{
        items?: {
          items: Array<{
            highlight?: (highlightedIndex: number) => void;
          }>;
        };
      }>;
    };

    touchBar.items[0]?.items?.items[0]?.highlight?.(1);

    expect(onAction).toHaveBeenCalledWith({
      type: "project.select",
      projectId: "project-2",
    });
  });

  it("uses the folder icon in project scrubber items", () => {
    const touchBar = buildTouchBar(
      {
        project: {
          label: "Select Project",
          items: [{ id: ProjectId.makeUnsafe("project-1"), label: "First" }],
        },
        activeProjectId: null,
        editor: null,
        git: null,
      },
      vi.fn(),
    ) as unknown as {
      items: Array<{
        items?: {
          items: Array<{
            items?: Array<{
              icon?: unknown;
            }>;
          }>;
        };
      }>;
    };

    expect(touchBar.items[0]?.items?.items[0]?.items?.[0]).toMatchObject({
      icon: { kind: "native-image" },
    });
  });

  it("shows arrow buttons for project scrubber overflow", () => {
    const touchBar = buildTouchBar(
      {
        project: {
          label: "Select Project",
          items: [
            { id: ProjectId.makeUnsafe("project-1"), label: "First" },
            { id: ProjectId.makeUnsafe("project-2"), label: "Second" },
          ],
        },
        activeProjectId: null,
        editor: null,
        git: null,
      },
      vi.fn(),
    ) as unknown as {
      items: Array<{
        items?: {
          items: Array<{
            mode?: string;
            showArrowButtons?: boolean;
          }>;
        };
      }>;
    };

    expect(touchBar.items[0]?.items?.items[0]).toMatchObject({
      mode: "free",
      showArrowButtons: true,
    });
  });

  it("emits project.newThread for the new-thread button", () => {
    const onAction = vi.fn();
    const touchBar = buildTouchBar(
      {
        project: {
          label: "Select Project",
          items: [{ id: ProjectId.makeUnsafe("project-1"), label: "First" }],
        },
        activeProjectId: ProjectId.makeUnsafe("project-1"),
        editor: null,
        git: null,
      },
      onAction,
    ) as unknown as {
      items: Array<{
        click?: () => void;
      }>;
    };

    touchBar.items[1]?.click?.();

    expect(onAction).toHaveBeenCalledWith({
      type: "project.newThread",
      projectId: "project-1",
    });
  });

  it("renders commit and push buttons when git state is present", () => {
    const touchBar = buildTouchBar(
      {
        project: null,
        activeProjectId: null,
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
        activeProjectId: null,
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
        activeProjectId: null,
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

  it("renders the preferred editor icon when an editor id is provided", () => {
    const touchBar = buildTouchBar(
      {
        project: null,
        activeProjectId: null,
        editor: {
          label: "Open",
          enabled: true,
          editorId: "vscode",
        },
        git: null,
      },
      vi.fn(),
    ) as unknown as {
      items: Array<{
        icon?: unknown;
      }>;
    };

    expect(touchBar.items[0]).toMatchObject({ icon: { kind: "native-image" } });
    expect(touchBarMocks.nativeImage.createFromPath).toHaveBeenCalled();
  });
});
