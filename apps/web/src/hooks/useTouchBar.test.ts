import { beforeEach, describe, expect, it, vi } from "vitest";
import { ProjectId, type GitStatusResult } from "@t3tools/contracts";

const testState = vi.hoisted(() => ({
  cleanups: [] as Array<(() => void) | undefined>,
  preferredEditor: "vscode" as string | null,
  serverConfig: {
    availableEditors: ["vscode"] as string[],
  },
  gitStatus: null as GitStatusResult | null,
  branchList: null as {
    isRepo: boolean;
    hasOriginRemote: boolean;
    branches: Array<{ name: string; current: boolean; isDefault: boolean }>;
  } | null,
  isRunStackedActionRunning: false,
  isPullRunning: false,
  hookValue: {
    activeDraftThread: null as { projectId: string; worktreePath?: string | null } | null,
    activeThread: undefined as { projectId: string; worktreePath?: string | null } | undefined,
    handleNewThread: vi.fn<(projectId: string) => Promise<void>>(),
    projects: [] as Array<{ id: string; name: string; cwd: string }>,
    routeThreadId: null,
  },
}));

function makeStatus(overrides: Partial<GitStatusResult> = {}): GitStatusResult {
  return {
    branch: "feature/test",
    hasWorkingTreeChanges: false,
    workingTree: {
      files: [],
      insertions: 0,
      deletions: 0,
    },
    hasUpstream: true,
    aheadCount: 0,
    behindCount: 0,
    pr: null,
    ...overrides,
  };
}

vi.mock("react", () => ({
  useEffect: (effect: () => void | (() => void)) => {
    testState.cleanups.push(effect() ?? undefined);
  },
}));

vi.mock("@tanstack/react-query", () => ({
  queryOptions: <T>(options: T) => options,
  useQuery: (options: { queryKey: readonly unknown[] }) => {
    const [scope, type] = options.queryKey;
    if (scope === "server" && type === "config") {
      return { data: testState.serverConfig, error: null };
    }
    if (scope === "git" && type === "status") {
      return { data: testState.gitStatus, error: null };
    }
    if (scope === "git" && type === "branches") {
      return { data: testState.branchList, error: null };
    }
    return { data: null, error: null };
  },
  useIsMutating: ({ mutationKey }: { mutationKey: readonly unknown[] }) => {
    const action = mutationKey[2];
    if (action === "run-stacked-action") {
      return testState.isRunStackedActionRunning ? 1 : 0;
    }
    if (action === "pull") {
      return testState.isPullRunning ? 1 : 0;
    }
    return 0;
  },
}));

vi.mock("./useHandleNewThread", () => ({
  useHandleNewThread: () => testState.hookValue,
}));

vi.mock("../editorPreferences", () => ({
  usePreferredEditor: () => [testState.preferredEditor, vi.fn()] as const,
  openInPreferredEditor: vi.fn(),
}));

vi.mock("../lib/gitReactQuery", () => ({
  gitStatusQueryOptions: (cwd: string | null) => ({
    queryKey: ["git", "status", cwd] as const,
  }),
  gitBranchesQueryOptions: (cwd: string | null) => ({
    queryKey: ["git", "branches", cwd] as const,
  }),
  gitMutationKeys: {
    runStackedAction: (cwd: string | null) => ["git", "mutation", "run-stacked-action", cwd],
    pull: (cwd: string | null) => ["git", "mutation", "pull", cwd],
  },
}));

function getWindowForTest(): Window & typeof globalThis & { desktopBridge?: unknown } {
  const testGlobal = globalThis as typeof globalThis & {
    window?: Window &
      typeof globalThis & {
        desktopBridge?: unknown;
        localStorage?: Storage;
      };
  };
  if (!testGlobal.window) {
    testGlobal.window = {} as Window & typeof globalThis & { desktopBridge?: unknown };
  }
  return testGlobal.window;
}

function createLocalStorageMock(): Storage {
  const store = new Map<string, string>();
  return {
    clear: () => store.clear(),
    getItem: (key) => store.get(key) ?? null,
    key: (index) => Array.from(store.keys())[index] ?? null,
    get length() {
      return store.size;
    },
    removeItem: (key) => {
      store.delete(key);
    },
    setItem: (key, value) => {
      store.set(key, value);
    },
  };
}

describe("useTouchBar", () => {
  beforeEach(() => {
    vi.resetModules();
    testState.cleanups = [];
    testState.preferredEditor = "vscode";
    testState.serverConfig = {
      availableEditors: ["vscode"],
    };
    testState.gitStatus = null;
    testState.branchList = null;
    testState.isRunStackedActionRunning = false;
    testState.isPullRunning = false;
    testState.hookValue.activeDraftThread = null;
    testState.hookValue.activeThread = undefined;
    testState.hookValue.handleNewThread.mockReset();
    testState.hookValue.projects = [];
    Object.defineProperty(getWindowForTest(), "localStorage", {
      configurable: true,
      writable: true,
      value: createLocalStorageMock(),
    });
    Reflect.deleteProperty(getWindowForTest(), "desktopBridge");
  });

  it('sends "Select Project" when there is no active thread or draft', async () => {
    const setTouchBarState = vi.fn().mockResolvedValue(undefined);
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: {
        setTouchBarState,
      },
    });

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();

    expect(setTouchBarState).toHaveBeenCalledWith({
      project: {
        label: "Select Project",
        items: [],
      },
      activeProjectId: null,
      editor: null,
      git: null,
    });
  });

  it("sends the active thread project name and project order", async () => {
    const setTouchBarState = vi.fn().mockResolvedValue(undefined);
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: {
        setTouchBarState,
      },
    });
    testState.hookValue.projects = [
      { id: ProjectId.makeUnsafe("project-2"), name: "Second", cwd: "/repo/second" },
      { id: ProjectId.makeUnsafe("project-1"), name: "First", cwd: "/repo/first" },
    ];
    testState.hookValue.activeThread = {
      projectId: ProjectId.makeUnsafe("project-1"),
    };

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();

    expect(setTouchBarState).toHaveBeenCalledWith({
      project: {
        label: "First",
        items: [
          { id: "project-2", label: "Second" },
          { id: "project-1", label: "First" },
        ],
      },
      activeProjectId: "project-1",
      editor: {
        label: "Open",
        enabled: true,
        editorId: "vscode",
      },
      git: {
        commitEnabled: false,
        pushEnabled: false,
      },
    });
  });

  it("falls back to the active draft project when no active thread exists", async () => {
    const setTouchBarState = vi.fn().mockResolvedValue(undefined);
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: {
        setTouchBarState,
      },
    });
    testState.hookValue.projects = [
      { id: ProjectId.makeUnsafe("project-1"), name: "First", cwd: "/repo/first" },
    ];
    testState.hookValue.activeDraftThread = {
      projectId: ProjectId.makeUnsafe("project-1"),
    };

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();

    expect(setTouchBarState).toHaveBeenCalledWith({
      project: {
        label: "First",
        items: [{ id: "project-1", label: "First" }],
      },
      activeProjectId: "project-1",
      editor: {
        label: "Open",
        enabled: true,
        editorId: "vscode",
      },
      git: {
        commitEnabled: false,
        pushEnabled: false,
      },
    });
  });

  it("publishes enabled commit state for dirty repos", async () => {
    const setTouchBarState = vi.fn().mockResolvedValue(undefined);
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: { setTouchBarState },
    });
    testState.hookValue.projects = [
      { id: ProjectId.makeUnsafe("project-1"), name: "First", cwd: "/repo/first" },
    ];
    testState.hookValue.activeThread = {
      projectId: ProjectId.makeUnsafe("project-1"),
      worktreePath: "/repo/first/.t3/worktrees/feature",
    };
    testState.gitStatus = makeStatus({ hasWorkingTreeChanges: true });
    testState.branchList = {
      isRepo: true,
      hasOriginRemote: true,
      branches: [{ name: "feature/test", current: true, isDefault: false }],
    };

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();

    expect(setTouchBarState).toHaveBeenCalledWith({
      project: {
        label: "First",
        items: [{ id: "project-1", label: "First" }],
      },
      activeProjectId: "project-1",
      editor: {
        label: "Open",
        enabled: true,
        editorId: "vscode",
      },
      git: {
        commitEnabled: true,
        pushEnabled: false,
      },
    });
  });

  it("publishes enabled push state for clean branches ahead of upstream", async () => {
    const setTouchBarState = vi.fn().mockResolvedValue(undefined);
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: { setTouchBarState },
    });
    testState.hookValue.projects = [
      { id: ProjectId.makeUnsafe("project-1"), name: "First", cwd: "/repo/first" },
    ];
    testState.hookValue.activeThread = {
      projectId: ProjectId.makeUnsafe("project-1"),
    };
    testState.gitStatus = makeStatus({ aheadCount: 2 });
    testState.branchList = {
      isRepo: true,
      hasOriginRemote: true,
      branches: [{ name: "feature/test", current: true, isDefault: false }],
    };

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();

    expect(setTouchBarState).toHaveBeenCalledWith({
      project: {
        label: "First",
        items: [{ id: "project-1", label: "First" }],
      },
      activeProjectId: "project-1",
      editor: {
        label: "Open",
        enabled: true,
        editorId: "vscode",
      },
      git: {
        commitEnabled: false,
        pushEnabled: true,
      },
    });
  });

  it("publishes disabled git actions while git is busy", async () => {
    const setTouchBarState = vi.fn().mockResolvedValue(undefined);
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: { setTouchBarState },
    });
    testState.hookValue.projects = [
      { id: ProjectId.makeUnsafe("project-1"), name: "First", cwd: "/repo/first" },
    ];
    testState.hookValue.activeThread = {
      projectId: ProjectId.makeUnsafe("project-1"),
    };
    testState.gitStatus = makeStatus({ hasWorkingTreeChanges: true });
    testState.branchList = {
      isRepo: true,
      hasOriginRemote: true,
      branches: [{ name: "feature/test", current: true, isDefault: false }],
    };
    testState.isRunStackedActionRunning = true;

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();

    expect(setTouchBarState).toHaveBeenCalledWith({
      project: {
        label: "First",
        items: [{ id: "project-1", label: "First" }],
      },
      activeProjectId: "project-1",
      editor: {
        label: "Open",
        enabled: true,
        editorId: "vscode",
      },
      git: {
        commitEnabled: false,
        pushEnabled: false,
      },
    });
  });

  it("hides git controls when the active cwd is not a repo", async () => {
    const setTouchBarState = vi.fn().mockResolvedValue(undefined);
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: { setTouchBarState },
    });
    testState.hookValue.projects = [
      { id: ProjectId.makeUnsafe("project-1"), name: "First", cwd: "/repo/first" },
    ];
    testState.hookValue.activeThread = {
      projectId: ProjectId.makeUnsafe("project-1"),
    };
    testState.branchList = {
      isRepo: false,
      hasOriginRemote: false,
      branches: [],
    };

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();

    expect(setTouchBarState).toHaveBeenCalledWith({
      project: {
        label: "First",
        items: [{ id: "project-1", label: "First" }],
      },
      activeProjectId: "project-1",
      editor: {
        label: "Open",
        enabled: true,
        editorId: "vscode",
      },
      git: null,
    });
  });

  it("updates the touch bar editor icon when the preferred editor changes", async () => {
    const setTouchBarState = vi.fn().mockResolvedValue(undefined);
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: {
        setTouchBarState,
      },
    });
    testState.hookValue.projects = [
      { id: ProjectId.makeUnsafe("project-1"), name: "First", cwd: "/repo/first" },
    ];
    testState.hookValue.activeThread = {
      projectId: ProjectId.makeUnsafe("project-1"),
    };

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();

    testState.preferredEditor = "cursor";
    useTouchBar();

    expect(setTouchBarState).toHaveBeenLastCalledWith({
      project: {
        label: "First",
        items: [{ id: "project-1", label: "First" }],
      },
      activeProjectId: "project-1",
      editor: {
        label: "Open",
        enabled: true,
        editorId: "cursor",
      },
      git: {
        commitEnabled: false,
        pushEnabled: false,
      },
    });
  });

  it("routes project selection actions through handleNewThread", async () => {
    let touchBarListener: ((action: { type: string; projectId?: string }) => void) | undefined;
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: {
        setTouchBarState: vi.fn().mockResolvedValue(undefined),
        onTouchBarAction: vi.fn((listener) => {
          touchBarListener = listener;
          return vi.fn();
        }),
      },
    });
    testState.hookValue.projects = [
      { id: ProjectId.makeUnsafe("project-1"), name: "First", cwd: "/repo/first" },
    ];

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();
    touchBarListener?.({ type: "project.select", projectId: "project-1" });

    expect(testState.hookValue.handleNewThread).toHaveBeenCalledWith("project-1");
  });

  it("routes new-thread touch bar actions through handleNewThread", async () => {
    let touchBarListener: ((action: { type: string; projectId?: string }) => void) | undefined;
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: {
        setTouchBarState: vi.fn().mockResolvedValue(undefined),
        onTouchBarAction: vi.fn((listener) => {
          touchBarListener = listener;
          return vi.fn();
        }),
      },
    });
    testState.hookValue.projects = [
      { id: ProjectId.makeUnsafe("project-1"), name: "First", cwd: "/repo/first" },
    ];

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();
    touchBarListener?.({ type: "project.newThread", projectId: "project-1" });

    expect(testState.hookValue.handleNewThread).toHaveBeenCalledWith("project-1");
  });

  it("ignores selecting the already active project", async () => {
    let touchBarListener: ((action: { type: string; projectId?: string }) => void) | undefined;
    const setTouchBarState = vi.fn().mockResolvedValue(undefined);
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: {
        setTouchBarState,
        onTouchBarAction: vi.fn((listener) => {
          touchBarListener = listener;
          return vi.fn();
        }),
      },
    });
    testState.hookValue.projects = [
      { id: ProjectId.makeUnsafe("project-1"), name: "First", cwd: "/repo/first" },
    ];
    testState.hookValue.activeThread = {
      projectId: ProjectId.makeUnsafe("project-1"),
    };

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();
    touchBarListener?.({ type: "project.select", projectId: "project-1" });

    expect(testState.hookValue.handleNewThread).not.toHaveBeenCalled();
    expect(setTouchBarState).toHaveBeenCalledTimes(1);
    expect(setTouchBarState).toHaveBeenCalledWith({
      project: {
        label: "First",
        items: [{ id: "project-1", label: "First" }],
      },
      activeProjectId: "project-1",
      editor: {
        label: "Open",
        enabled: true,
        editorId: "vscode",
      },
      git: {
        commitEnabled: false,
        pushEnabled: false,
      },
    });
  });

  it("ignores project selection for missing projects", async () => {
    let touchBarListener: ((action: { type: string; projectId?: string }) => void) | undefined;
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: {
        setTouchBarState: vi.fn().mockResolvedValue(undefined),
        onTouchBarAction: vi.fn((listener) => {
          touchBarListener = listener;
          return vi.fn();
        }),
      },
    });
    testState.hookValue.projects = [
      { id: ProjectId.makeUnsafe("project-1"), name: "First", cwd: "/repo/first" },
    ];

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();
    touchBarListener?.({ type: "project.select", projectId: "missing-project" });

    expect(testState.hookValue.handleNewThread).not.toHaveBeenCalled();
  });

  it("does nothing when the desktop bridge is unavailable", async () => {
    const { useTouchBar } = await import("./useTouchBar");

    expect(() => useTouchBar()).not.toThrow();
    expect(testState.hookValue.handleNewThread).not.toHaveBeenCalled();
  });

  it("clears the touch bar state on unmount", async () => {
    const setTouchBarState = vi.fn().mockResolvedValue(undefined);
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: {
        setTouchBarState,
        onTouchBarAction: vi.fn(() => vi.fn()),
      },
    });

    const { useTouchBar } = await import("./useTouchBar");
    useTouchBar();

    for (const cleanup of testState.cleanups) {
      cleanup?.();
    }

    expect(setTouchBarState).toHaveBeenLastCalledWith({
      project: null,
      activeProjectId: null,
      editor: null,
      git: null,
    });
  });
});
