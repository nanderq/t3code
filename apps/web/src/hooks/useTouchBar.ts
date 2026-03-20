import type { TouchBarState } from "@t3tools/contracts";
import { useIsMutating, useQuery } from "@tanstack/react-query";
import { useEffect } from "react";
import { deriveGitActionAvailability } from "../components/GitActionsControl.logic";
import { openInPreferredEditor, usePreferredEditor } from "../editorPreferences";
import { useHandleNewThread } from "./useHandleNewThread";
import {
  gitBranchesQueryOptions,
  gitMutationKeys,
  gitStatusQueryOptions,
} from "../lib/gitReactQuery";
import { serverConfigQueryOptions } from "../lib/serverReactQuery";
import { readNativeApi } from "../nativeApi";

const EMPTY_TOUCH_BAR_STATE: TouchBarState = {
  project: null,
  activeProjectId: null,
  editor: null,
  git: null,
};

type TouchBarProjectId = NonNullable<TouchBarState["project"]>["items"][number]["id"];
type TouchBarEditorId = NonNullable<TouchBarState["editor"]>["editorId"];

function createTouchBarState({
  activeProject,
  projects,
  openInCwd,
  preferredEditor,
  gitCwd,
  isRepo,
  gitAvailability,
}: {
  activeProject: { id: TouchBarProjectId; name: string } | undefined;
  projects: Array<{ id: TouchBarProjectId; name: string }>;
  openInCwd: string | null | undefined;
  preferredEditor: TouchBarEditorId | null;
  gitCwd: string | null | undefined;
  isRepo: boolean | undefined;
  gitAvailability: { canCommit: boolean; canPush: boolean } | null;
}): TouchBarState {
  return {
    project: {
      label: activeProject?.name ?? "Select Project",
      items: projects.map((project) => ({
        id: project.id,
        label: project.name,
      })),
    },
    activeProjectId: activeProject?.id ?? null,
    editor:
      openInCwd === undefined || openInCwd === null
        ? null
        : {
            label: "Open",
            enabled: preferredEditor !== null,
            ...(preferredEditor ? { editorId: preferredEditor } : {}),
          },
    git:
      gitCwd === undefined || gitCwd === null || isRepo === false || !gitAvailability
        ? null
        : {
            commitEnabled: gitAvailability.canCommit,
            pushEnabled: gitAvailability.canPush,
          },
  };
}

export function useTouchBar(): void {
  const { activeDraftThread, activeThread, handleNewThread, projects } = useHandleNewThread();
  const activeProjectId = activeThread?.projectId ?? activeDraftThread?.projectId ?? null;
  const activeProject = projects.find((project) => project.id === activeProjectId);
  const openInCwd =
    activeThread?.worktreePath ?? activeDraftThread?.worktreePath ?? activeProject?.cwd;
  const { data: serverConfig = null } = useQuery(serverConfigQueryOptions());
  const [preferredEditor] = usePreferredEditor(serverConfig?.availableEditors ?? []);
  const gitCwd = openInCwd;
  const { data: gitStatus = null } = useQuery(gitStatusQueryOptions(gitCwd ?? null));
  const { data: branchList = null } = useQuery(gitBranchesQueryOptions(gitCwd ?? null));
  const isRunStackedActionRunning =
    useIsMutating({ mutationKey: gitMutationKeys.runStackedAction(gitCwd ?? null) }) > 0;
  const isPullRunning = useIsMutating({ mutationKey: gitMutationKeys.pull(gitCwd ?? null) }) > 0;
  const gitAvailability =
    gitCwd === undefined || gitCwd === null
      ? null
      : deriveGitActionAvailability(
          gitStatus,
          isRunStackedActionRunning || isPullRunning,
          branchList?.hasOriginRemote ?? false,
          branchList?.isRepo ?? true,
        );

  useEffect(() => {
    const bridge = window.desktopBridge;
    if (typeof bridge?.setTouchBarState !== "function") {
      return;
    }

    void bridge.setTouchBarState(
      createTouchBarState({
        activeProject,
        projects,
        openInCwd,
        preferredEditor,
        gitCwd,
        isRepo: branchList?.isRepo,
        gitAvailability,
      }),
    );
  }, [
    activeProject,
    activeProject?.id,
    activeProject?.name,
    branchList?.isRepo,
    gitAvailability,
    gitCwd,
    openInCwd,
    preferredEditor,
    projects,
  ]);

  useEffect(() => {
    const bridge = window.desktopBridge;
    if (typeof bridge?.onTouchBarAction !== "function") {
      return;
    }

    return bridge.onTouchBarAction((action) => {
      if (action.type === "project.newThread") {
        if (!projects.some((project) => project.id === action.projectId)) {
          return;
        }
        void handleNewThread(action.projectId);
        return;
      }

      if (action.type === "project.select") {
        if (!projects.some((project) => project.id === action.projectId)) {
          return;
        }
        if (action.projectId === activeProjectId) {
          return;
        }
        void handleNewThread(action.projectId);
        return;
      }

      if (action.type !== "editor.openPreferred" || !openInCwd) {
        return;
      }

      const api = readNativeApi();
      if (!api) return;
      void openInPreferredEditor(api, openInCwd).catch(() => undefined);
    });
  }, [
    activeProject,
    activeProject?.id,
    activeProject?.name,
    activeProjectId,
    branchList?.isRepo,
    gitAvailability,
    gitCwd,
    handleNewThread,
    openInCwd,
    preferredEditor,
    projects,
  ]);

  useEffect(() => {
    const bridge = window.desktopBridge;
    if (typeof bridge?.setTouchBarState !== "function") {
      return;
    }

    return () => {
      void bridge.setTouchBarState(EMPTY_TOUCH_BAR_STATE);
    };
  }, []);
}
