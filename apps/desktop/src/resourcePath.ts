import * as FS from "node:fs";
import * as Path from "node:path";

export function resolveDesktopResourcePath(fileName: string): string | null {
  const candidates = [
    Path.join(__dirname, "../resources", fileName),
    Path.join(__dirname, "../prod-resources", fileName),
  ];
  const resourcesPath = process.resourcesPath;

  if (resourcesPath) {
    candidates.push(
      Path.join(resourcesPath, "resources", fileName),
      Path.join(resourcesPath, fileName),
    );
  }

  for (const candidate of candidates) {
    if (FS.existsSync(candidate)) {
      return candidate;
    }
  }

  return null;
}

export function resolveDesktopIconPath(ext: "ico" | "icns" | "png"): string | null {
  return resolveDesktopResourcePath(`icon.${ext}`);
}
