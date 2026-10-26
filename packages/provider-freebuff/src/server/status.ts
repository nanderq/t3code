import type { ServerProviderModel } from "@t3tools/contracts";
import * as HostProcess from "@t3tools/shared/HostProcess";
import { createModelCapabilities } from "@t3tools/shared/model";
import { compareSemverVersions } from "@t3tools/shared/semver";
import { resolveSpawnCommand } from "@t3tools/shared/shell";
import * as DateTime from "effect/DateTime";
import * as Effect from "effect/Effect";
import * as Option from "effect/Option";
import * as Result from "effect/Result";
import * as ChildProcess from "effect/process/ChildProcess";
import * as ChildProcessSpawner from "effect/process/ChildProcessSpawner";

import {
  buildServerProvider,
  DEFAULT_TIMEOUT_MS,
  isCommandMissingCause,
  parseGenericCliVersion,
  spawnAndCollect,
  type ProviderProbeResult,
  type ServerProviderDraft,
} from "@t3tools/provider-core/server/snapshotProbe";
import type { FreebuffSettings } from "../settings.ts";

const FREEBUFF_PRESENTATION = {
  displayName: "Freebuff",
  showInteractionModeToggle: false,
  supportsConversationRollback: false,
  supportedRuntimeModes: ["full-access"],
} as const;

export const MINIMUM_FREEBUFF_VERSION = "0.2.34";

// The native CLI selects its own model and does not expose model switching.
const FREEBUFF_MODELS: ReadonlyArray<ServerProviderModel> = [
  {
    slug: "freebuff",
    name: "Freebuff",
    isCustom: false,
    isDefault: true,
    capabilities: createModelCapabilities({ optionDescriptors: [] }),
  },
];

export const buildInitialFreebuffProviderSnapshot = Effect.fn(
  "FreebuffProviderStatus.buildInitialFreebuffProviderSnapshot",
)(function* (settings: FreebuffSettings): Effect.fn.Return<ServerProviderDraft> {
  const checkedAt = DateTime.formatIso(yield* DateTime.now);
  return buildServerProvider({
    presentation: FREEBUFF_PRESENTATION,
    enabled: settings.enabled,
    checkedAt,
    models: FREEBUFF_MODELS,
    probe: {
      installed: settings.enabled,
      version: null,
      status: "warning",
      auth: { status: "unknown" },
      message: settings.enabled
        ? "Checking Freebuff CLI availability..."
        : "Freebuff is disabled in T3 Code settings.",
    },
  });
});

const runFreebuffVersionCommand = Effect.fn("FreebuffProviderStatus.runFreebuffVersionCommand")(
  function* (settings: FreebuffSettings, environment: NodeJS.ProcessEnv, cwd: string | undefined) {
    const command = settings.binaryPath || "freebuff";
    const spawnCommand = yield* resolveSpawnCommand(command, ["--version"], { env: environment });
    return yield* spawnAndCollect(
      command,
      ChildProcess.make(spawnCommand.command, spawnCommand.args, {
        env: environment,
        cwd,
        shell: spawnCommand.shell,
      }),
    );
  },
);

export const checkFreebuffProviderStatus = Effect.fn("checkFreebuffProviderStatus")(function* (
  settings: FreebuffSettings,
  environment?: NodeJS.ProcessEnv,
  cwd?: string,
): Effect.fn.Return<ServerProviderDraft, never, ChildProcessSpawner.ChildProcessSpawner> {
  if (!settings.enabled) {
    return yield* buildInitialFreebuffProviderSnapshot(settings);
  }

  const checkedAt = DateTime.formatIso(yield* DateTime.now);
  const buildSnapshot = (probe: ProviderProbeResult) =>
    buildServerProvider({
      presentation: FREEBUFF_PRESENTATION,
      enabled: settings.enabled,
      checkedAt,
      models: FREEBUFF_MODELS,
      probe,
    });
  const processEnvironment = environment ?? (yield* HostProcess.Environment);
  const versionResult = yield* runFreebuffVersionCommand(settings, processEnvironment, cwd).pipe(
    Effect.timeoutOption(DEFAULT_TIMEOUT_MS),
    Effect.result,
  );

  if (Result.isFailure(versionResult)) {
    const missing = isCommandMissingCause(versionResult.failure);
    yield* Effect.logWarning("Freebuff CLI health check failed.", {
      errorTag: versionResult.failure._tag,
    });
    return buildSnapshot({
      installed: !missing,
      version: null,
      status: "error",
      auth: { status: "unknown" },
      message: missing
        ? "Freebuff CLI (`freebuff`) is not installed or not on PATH. Install with `npm install -g freebuff`."
        : "Failed to execute Freebuff CLI health check.",
    });
  }

  if (Option.isNone(versionResult.success)) {
    return buildSnapshot({
      installed: true,
      version: null,
      status: "error",
      auth: { status: "unknown" },
      message: "Freebuff CLI is installed but timed out while running `freebuff --version`.",
    });
  }

  const output = versionResult.success.value;
  const version = parseGenericCliVersion(`${output.stdout}\n${output.stderr}`);
  if (output.code !== 0) {
    return buildSnapshot({
      installed: true,
      version,
      status: "error",
      auth: { status: "unknown" },
      message: "Freebuff CLI is installed but failed to run.",
    });
  }

  if (version === null || compareSemverVersions(version, MINIMUM_FREEBUFF_VERSION) < 0) {
    return buildSnapshot({
      installed: true,
      version,
      status: "error",
      auth: { status: "unknown" },
      message: `Freebuff ${MINIMUM_FREEBUFF_VERSION} or newer is required. Update with \`npm install -g freebuff\`.`,
    });
  }

  // A successful version check proves availability, not the native account's login state.
  return buildSnapshot({
    installed: true,
    version,
    status: "ready",
    auth: { status: "unknown" },
  });
});
