import { nativeImage } from "electron";
import type { EditorId } from "@t3tools/contracts";
import { resolveDesktopResourcePath } from "./resourcePath";

const TOUCH_BAR_ICON_SIZE = { width: 16, height: 16 } as const;

const editorIconCache = new Map<EditorId, Electron.NativeImage | null>();

function resizeTouchBarIcon(image: Electron.NativeImage): Electron.NativeImage {
  return image.resize(TOUCH_BAR_ICON_SIZE);
}

export function createTouchBarIcon(iconName: string): Electron.NativeImage | undefined {
  const iconPath = resolveDesktopResourcePath(`touchBarIcons/${iconName}.png`);
  if (!iconPath) {
    return undefined;
  }

  return resizeTouchBarIcon(nativeImage.createFromPath(iconPath));
}

export function createEditorTouchBarIcon(
  editorId: EditorId | undefined,
): Electron.NativeImage | undefined {
  if (!editorId) {
    return undefined;
  }

  if (editorId === "file-manager") {
    return createTouchBarIcon("folder");
  }

  if (editorIconCache.has(editorId)) {
    return editorIconCache.get(editorId) ?? undefined;
  }

  const icon = createTouchBarIcon(editorId) ?? null;
  editorIconCache.set(editorId, icon);

  return icon ?? undefined;
}
