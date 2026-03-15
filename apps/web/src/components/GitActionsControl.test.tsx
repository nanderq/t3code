import { ThreadId, type GitRunStackedActionResult, type GitStatusResult } from "@t3tools/contracts";
import { beforeEach, describe, expect, it, vi } from "vitest";

const testState = vi.hoisted(() => ({
  cleanups: [] as Array<(() => void) | undefined>,
  touchBarListener: undefined as ((action: { type: string }) => void) | undefined,
  gitStatus: null as GitStatusResult | null,
  branchList: {
    isRepo: true,
    hasOriginRemote: true,
    branches: [{ name: "feature/test", current: true, isDefault: false }],
  },
  isRunStackedActionRunning: false,
  isPullRunning: false,
  runStackedActionMutateAsync:
    vi.fn<
      (input: {
        action: string;
        commitMessage?: string;
        featureBranch?: boolean;
        filePaths?: string[];
      }) => Promise<GitRunStackedActionResult>
    >(),
  pullMutateAsync:
    vi.fn<() => Promise<{ status: "pulled"; branch: string; upstreamBranch: string }>>(),
  initMutate: vi.fn(),
  invalidateGitQueries: vi.fn().mockResolvedValue(undefined),
  toastAdd: vi.fn(() => "toast-id"),
  toastUpdate: vi.fn(),
  toastClose: vi.fn(),
  toastPromise: vi.fn(),
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

function makeResult(action: "commit" | "commit_push"): GitRunStackedActionResult {
  return {
    action,
    branch: { status: "skipped_not_requested" },
    commit: {
      status: action === "commit" ? "created" : "skipped_no_changes",
      ...(action === "commit" ? { commitSha: "abcdef123456", subject: "Generated commit" } : {}),
    },
    push: {
      status: action === "commit_push" ? "pushed" : "skipped_not_requested",
      ...(action === "commit_push"
        ? { branch: "feature/test", upstreamBranch: "origin/feature/test", setUpstream: false }
        : {}),
    },
    pr: { status: "skipped_not_requested" },
  };
}

vi.mock("react", async () => {
  const actual = await vi.importActual<typeof import("react")>("react");
  return {
    ...actual,
    useCallback: ((fn: (...args: never[]) => unknown) => fn) as <
      T extends (...args: never[]) => unknown,
    >(
      fn: T,
    ) => T,
    useEffect: (effect: () => void | (() => void)) => {
      testState.cleanups.push(effect() ?? undefined);
    },
    useMemo: ((factory: () => unknown) => factory()) as <T>(factory: () => T) => T,
    useState: ((initial: unknown) => [
      typeof initial === "function" ? (initial as () => unknown)() : initial,
      vi.fn(),
    ]) as <T>(initial: T | (() => T)) => readonly [T, ReturnType<typeof vi.fn>],
  };
});

vi.mock("@tanstack/react-query", () => ({
  useQuery: (options: { queryKey: readonly unknown[] }) => {
    const [scope, type] = options.queryKey;
    if (scope === "git" && type === "status") {
      return { data: testState.gitStatus, error: null };
    }
    if (scope === "git" && type === "branches") {
      return { data: testState.branchList, error: null };
    }
    return { data: null, error: null };
  },
  useMutation: (options: { mutationKey?: readonly unknown[] }) => {
    const action = options.mutationKey?.[2];
    if (action === "run-stacked-action") {
      return {
        mutateAsync: testState.runStackedActionMutateAsync,
        isPending: false,
      };
    }
    if (action === "pull") {
      return {
        mutateAsync: testState.pullMutateAsync,
        isPending: false,
      };
    }
    return {
      mutate: testState.initMutate,
      isPending: false,
    };
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
  useQueryClient: () => ({}),
}));

vi.mock("~/lib/gitReactQuery", () => ({
  gitStatusQueryOptions: (cwd: string | null) => ({
    queryKey: ["git", "status", cwd] as const,
  }),
  gitBranchesQueryOptions: (cwd: string | null) => ({
    queryKey: ["git", "branches", cwd] as const,
  }),
  gitInitMutationOptions: ({ cwd }: { cwd: string | null }) => ({
    mutationKey: ["git", "mutation", "init", cwd] as const,
  }),
  gitRunStackedActionMutationOptions: ({ cwd }: { cwd: string | null }) => ({
    mutationKey: ["git", "mutation", "run-stacked-action", cwd] as const,
  }),
  gitPullMutationOptions: ({ cwd }: { cwd: string | null }) => ({
    mutationKey: ["git", "mutation", "pull", cwd] as const,
  }),
  invalidateGitQueries: testState.invalidateGitQueries,
  gitMutationKeys: {
    runStackedAction: (cwd: string | null) => ["git", "mutation", "run-stacked-action", cwd],
    pull: (cwd: string | null) => ["git", "mutation", "pull", cwd],
  },
}));

vi.mock("~/components/ui/toast", () => ({
  toastManager: {
    add: testState.toastAdd,
    update: testState.toastUpdate,
    close: testState.toastClose,
    promise: testState.toastPromise,
  },
}));

vi.mock("~/nativeApi", () => ({
  readNativeApi: () => undefined,
}));

function getWindowForTest(): Window & typeof globalThis & { desktopBridge?: unknown } {
  const testGlobal = globalThis as typeof globalThis & {
    window?: Window & typeof globalThis & { desktopBridge?: unknown };
  };
  if (!testGlobal.window) {
    testGlobal.window = {} as Window & typeof globalThis & { desktopBridge?: unknown };
  }
  return testGlobal.window;
}

describe("GitActionsControl touch bar integration", () => {
  beforeEach(() => {
    vi.resetModules();
    testState.cleanups = [];
    testState.touchBarListener = undefined;
    testState.gitStatus = makeStatus();
    testState.branchList = {
      isRepo: true,
      hasOriginRemote: true,
      branches: [{ name: "feature/test", current: true, isDefault: false }],
    };
    testState.isRunStackedActionRunning = false;
    testState.isPullRunning = false;
    testState.runStackedActionMutateAsync.mockReset();
    testState.pullMutateAsync.mockReset();
    testState.initMutate.mockReset();
    testState.invalidateGitQueries.mockClear();
    testState.toastAdd.mockClear();
    testState.toastUpdate.mockClear();
    testState.toastClose.mockClear();
    testState.toastPromise.mockClear();
    testState.runStackedActionMutateAsync.mockResolvedValue(makeResult("commit"));
    Object.defineProperty(getWindowForTest(), "desktopBridge", {
      configurable: true,
      writable: true,
      value: {
        onTouchBarAction: vi.fn((listener: (action: { type: string }) => void) => {
          testState.touchBarListener = listener;
          return vi.fn();
        }),
      },
    });
  });

  it("runs immediate commit for touch bar commit actions", async () => {
    testState.gitStatus = makeStatus({ hasWorkingTreeChanges: true });
    const { default: GitActionsControl } = await import("./GitActionsControl");

    GitActionsControl({
      gitCwd: "/repo/project",
      activeThreadId: ThreadId.makeUnsafe("thread-id"),
    });

    testState.touchBarListener?.({ type: "git.commit" });
    await Promise.resolve();

    expect(testState.runStackedActionMutateAsync).toHaveBeenCalledWith({
      action: "commit",
    });
  });

  it("runs push through the existing commit_push path for touch bar push actions", async () => {
    testState.gitStatus = makeStatus({ aheadCount: 2 });
    testState.runStackedActionMutateAsync.mockResolvedValue(makeResult("commit_push"));
    const { default: GitActionsControl } = await import("./GitActionsControl");

    GitActionsControl({
      gitCwd: "/repo/project",
      activeThreadId: ThreadId.makeUnsafe("thread-id"),
    });

    testState.touchBarListener?.({ type: "git.push" });
    await Promise.resolve();

    expect(testState.runStackedActionMutateAsync).toHaveBeenCalledWith({
      action: "commit_push",
    });
  });

  it("ignores touch bar actions when they are unavailable", async () => {
    testState.gitStatus = makeStatus({ aheadCount: 0, hasWorkingTreeChanges: false });
    const { default: GitActionsControl } = await import("./GitActionsControl");

    GitActionsControl({
      gitCwd: "/repo/project",
      activeThreadId: ThreadId.makeUnsafe("thread-id"),
    });

    testState.touchBarListener?.({ type: "git.push" });
    testState.touchBarListener?.({ type: "git.commit" });
    await Promise.resolve();

    expect(testState.runStackedActionMutateAsync).not.toHaveBeenCalled();
  });

  it("ignores touch bar actions while a git action is already running", async () => {
    testState.gitStatus = makeStatus({ hasWorkingTreeChanges: true });
    testState.isRunStackedActionRunning = true;
    const { default: GitActionsControl } = await import("./GitActionsControl");

    GitActionsControl({
      gitCwd: "/repo/project",
      activeThreadId: ThreadId.makeUnsafe("thread-id"),
    });

    testState.touchBarListener?.({ type: "git.commit" });
    await Promise.resolve();

    expect(testState.runStackedActionMutateAsync).not.toHaveBeenCalled();
  });
});
