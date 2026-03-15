import { TouchBar } from "electron";
import type { TouchBarAction, TouchBarState } from "@t3tools/contracts";

export function buildTouchBar(
  state: TouchBarState,
  onAction: (action: TouchBarAction) => void,
): Electron.TouchBar {
  const { TouchBarButton, TouchBarGroup, TouchBarPopover, TouchBarSpacer } = TouchBar;
  const items: ConstructorParameters<typeof TouchBar>[0]["items"] = [];

  if (state.project) {
    items.push(
      new TouchBarPopover({
        label: state.project.label,
        showCloseButton: true,
        items: new TouchBar({
          items: state.project.items.map(
            (project) =>
              new TouchBarButton({
                label: project.label,
                click: () => onAction({ type: "project.select", projectId: project.id }),
              }),
          ),
        }),
      }),
    );
  }

  if (state.project && (state.editor || state.git)) {
    items.push(new TouchBarSpacer({ size: "small" }));
  }

  if (state.editor) {
    items.push(
      new TouchBarButton({
        label: state.editor.label,
        enabled: state.editor.enabled,
        click: () => onAction({ type: "editor.openPreferred" }),
      }),
    );
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
