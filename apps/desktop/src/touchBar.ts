import { TouchBar } from "electron";
import type { TouchBarAction, TouchBarState } from "@t3tools/contracts";
import { createEditorTouchBarIcon, createTouchBarIcon } from "./touchBarIcon";

export function buildTouchBar(
  state: TouchBarState,
  onAction: (action: TouchBarAction) => void,
): Electron.TouchBar {
  const { TouchBarButton, TouchBarGroup, TouchBarPopover, TouchBarSpacer } = TouchBar;
  const TouchBarScrubber = (
    TouchBar as typeof TouchBar & {
      TouchBarScrubber: new (options: {
        items: Array<{ label?: string; icon?: Electron.NativeImage }>;
        mode?: string;
        showArrowButtons?: boolean;
        highlight?: (highlightedIndex: number) => void;
      }) => Electron.TouchBarScrubber;
    }
  ).TouchBarScrubber;
  const items: ConstructorParameters<typeof TouchBar>[0]["items"] = [];
  const projectIcon = createTouchBarIcon("folder");

  if (state.project) {
    const projectPopoverOptions: ConstructorParameters<typeof TouchBarPopover>[0] = {
      label: state.project.label,
      showCloseButton: true,
      items: new TouchBar({
        items: [
          new TouchBarScrubber({
            items: state.project.items.map((project) => ({
              label: project.label,
              ...(projectIcon ? { icon: projectIcon } : {}),
            })),
            mode: "free",
            showArrowButtons: state.project.items.length > 1,
            highlight: (highlightedIndex) => {
              const project = state.project?.items[highlightedIndex];
              if (!project) {
                return;
              }

              onAction({ type: "project.select", projectId: project.id });
            },
          }),
        ],
      }),
    };

    if (projectIcon) {
      projectPopoverOptions.icon = projectIcon;
    }

    items.push(new TouchBarPopover(projectPopoverOptions));

    if (state.activeProjectId) {
      const newThreadButtonOptions: ConstructorParameters<typeof TouchBarButton>[0] = {
        accessibilityLabel: "New Thread",
        click: () => onAction({ type: "project.newThread", projectId: state.activeProjectId! }),
      };
      const newThreadIcon = createTouchBarIcon("plus");

      if (newThreadIcon) {
        newThreadButtonOptions.icon = newThreadIcon;
      }

      items.push(new TouchBarButton(newThreadButtonOptions));
    }
  }

  if (state.project && (state.editor || state.git)) {
    items.push(new TouchBarSpacer({ size: "small" }));
  }

  if (state.editor) {
    const editorButtonOptions: ConstructorParameters<typeof TouchBarButton>[0] = {
      label: state.editor.label,
      enabled: state.editor.enabled,
      click: () => onAction({ type: "editor.openPreferred" }),
    };
    const editorIcon = createEditorTouchBarIcon(state.editor.editorId);

    if (editorIcon) {
      editorButtonOptions.icon = editorIcon;
      editorButtonOptions.iconPosition = "left";
    }

    items.push(new TouchBarButton(editorButtonOptions));
  }

  if (state.editor && state.git) {
    items.push(new TouchBarSpacer({ size: "flexible" }));
  }

  if (state.git) {
    items.push(
      new TouchBarGroup({
        items: new TouchBar({
          items: [
            new TouchBarButton({
              label: "Commit",
              enabled: state.git.commitEnabled,
              click: () => onAction({ type: "git.commit" }),
            }),
            new TouchBarButton({
              label: "Push",
              enabled: state.git.pushEnabled,
              click: () => onAction({ type: "git.push" }),
            }),
          ],
        }),
      }),
    );
  }

  return new TouchBar({ items });
}
