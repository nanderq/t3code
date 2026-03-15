import type { TouchBarState } from "@t3tools/contracts";
import { useIsMutating, useQuery } from "@tanstack/react-query";
import { useEffect } from "react";
import { deriveGitActionAvailability } from "../components/GitActionsControl.logic";
import { openInPreferredEditor } from "../editorPreferences";
import { useHandleNewThread } from "./useHandleNewThread";
import {
  gitBranchesQueryOptions,
  gitMutationKeys,
  gitStatusQueryOptions,
} from "../lib/gitReactQuery";
import { readNativeApi } from "../nativeApi";

const EMPTY_TOUCH_BAR_STATE: TouchBarState = {
  project: null,
  editor: null,
  git: null,
};

export function useTouchBar(): void {
  const { activeDraftThread, activeThread, handleNewThread, projects } = useHandleNewThread();
  const activeProjectId = activeThread?.projectId ?? activeDraftThread?.projectId ?? null;
  const activeProject = projects.find((project) => project.id === activeProjectId);
  const openInCwd =
    activeThread?.worktreePath ?? activeDraftThread?.worktreePath ?? activeProject?.cwd;
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

    void bridge.setTouchBarState({
      project: {
        label: activeProject?.name ?? "Select Project",
        items: projects.map((project) => ({
          id: project.id,
          label: project.name,
        })),
      },
      editor:
        openInCwd === undefined || openInCwd === null
          ? null
          : {
              label: "Open",
              enabled: true,
            },
      git:
        gitCwd === undefined || gitCwd === null || branchList?.isRepo === false || !gitAvailability
          ? null
          : {
              commitEnabled: gitAvailability.canCommit,
              pushEnabled: gitAvailability.canPush,
            },
    });
  }, [activeProject?.name, branchList?.isRepo, gitAvailability, gitCwd, openInCwd, projects]);

  useEffect(() => {
    const onTouchBarAction = window.desktopBridge?.onTouchBarAction;
    if (typeof onTouchBarAction !== "function") {
      return;
    }

    return onTouchBarAction((action) => {
      if (action.type === "project.select") {
        if (!projects.some((project) => project.id === action.projectId)) {
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
  }, [handleNewThread, openInCwd, projects]);

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
