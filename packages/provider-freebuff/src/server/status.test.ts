import { describe, expect, it } from "@effect/vitest";
import * as HostProcess from "@t3tools/shared/HostProcess";
import * as Deferred from "effect/Deferred";
import * as Effect from "effect/Effect";
import * as Fiber from "effect/Fiber";
import * as PlatformError from "effect/PlatformError";
import * as Schema from "effect/Schema";
import * as Sink from "effect/Sink";
import * as Stream from "effect/Stream";
import * as TestClock from "effect/testing/TestClock";
import * as ChildProcess from "effect/process/ChildProcess";
import * as ChildProcessSpawner from "effect/process/ChildProcessSpawner";

import { FreebuffSettings } from "../settings.ts";
import {
  buildInitialFreebuffProviderSnapshot,
  checkFreebuffProviderStatus,
  MINIMUM_FREEBUFF_VERSION,
} from "./status.ts";

const settings = Schema.decodeSync(FreebuffSettings)({});

function processHandle(input: {
  readonly stdout?: string;
  readonly stderr?: string;
  readonly exitCode?: number;
}) {
  return ChildProcessSpawner.makeHandle({
    pid: ChildProcessSpawner.ProcessId(900_000_001),
    exitCode: Effect.succeed(ChildProcessSpawner.ExitCode(input.exitCode ?? 0)),
    isRunning: Effect.succeed(false),
    kill: () => Effect.void,
    unref: Effect.succeed(Effect.void),
    stdin: Sink.drain,
    stdout: Stream.encodeText(Stream.succeed(input.stdout ?? "")),
    stderr: Stream.encodeText(Stream.succeed(input.stderr ?? "")),
    all: Stream.empty,
    getInputFd: () => Sink.drain,
    getOutputFd: () => Stream.empty,
  });
}

describe("FreebuffProvider", () => {
  it.effect("keeps the initial snapshot pending without claiming authentication", () =>
    Effect.gen(function* () {
      const snapshot = yield* buildInitialFreebuffProviderSnapshot(settings);
      expect(snapshot).toMatchObject({
        enabled: true,
        installed: true,
        status: "warning",
        version: null,
        auth: { status: "unknown" },
        message: "Checking Freebuff CLI availability...",
        showInteractionModeToggle: false,
        supportsConversationRollback: false,
        supportedRuntimeModes: ["full-access"],
        models: [{ slug: "freebuff", name: "Freebuff", isCustom: false, isDefault: true }],
      });
      expect(snapshot.slashCommands).toEqual([]);
    }),
  );

  it.effect("does not spawn a process for a disabled instance", () =>
    Effect.gen(function* () {
      const disabled = { ...settings, enabled: false };
      const initial = yield* buildInitialFreebuffProviderSnapshot(disabled);
      const snapshot = yield* checkFreebuffProviderStatus(disabled, {}).pipe(
        Effect.provideService(
          ChildProcessSpawner.ChildProcessSpawner,
          ChildProcessSpawner.make(() => Effect.die("Disabled providers must not spawn.")),
        ),
      );
      expect(initial).toMatchObject({
        enabled: false,
        installed: false,
        status: "disabled",
        auth: { status: "unknown" },
      });
      expect(snapshot).toMatchObject({
        enabled: false,
        installed: false,
        status: "disabled",
        message: "Freebuff is disabled in T3 Code settings.",
      });
    }),
  );

  it.effect.each([{ stdout: "0.2.34\n" }, { stderr: "freebuff v0.2.34\n" }])(
    "reads the CLI version while leaving authentication unknown: %j",
    (output) =>
      Effect.gen(function* () {
        const commands: Array<ChildProcess.Command> = [];
        const environment = { PATH: "/freebuff/bin", FREEBUFF_TEST_VALUE: "instance" };
        const snapshot = yield* checkFreebuffProviderStatus(
          { ...settings, binaryPath: "/freebuff/bin/freebuff" },
          environment,
          "/workspace",
        ).pipe(
          Effect.provideService(HostProcess.Platform, "linux"),
          Effect.provideService(
            ChildProcessSpawner.ChildProcessSpawner,
            ChildProcessSpawner.make((command) => {
              commands.push(command);
              return Effect.succeed(processHandle(output));
            }),
          ),
        );
        expect(snapshot).toMatchObject({
          installed: true,
          status: "ready",
          version: "0.2.34",
          auth: { status: "unknown" },
          setup: { canAuthenticate: false, canInstall: false },
        });
        expect(commands).toHaveLength(1);
        expect(commands[0]).toMatchObject({
          command: "/freebuff/bin/freebuff",
          args: ["--version"],
          options: { env: environment, cwd: "/workspace", shell: false },
        });
      }),
  );

  it.effect("advertises only the native default model despite custom model settings", () =>
    Effect.gen(function* () {
      const snapshot = yield* checkFreebuffProviderStatus(
        {
          ...settings,
          customModels: [
            { slug: "custom-agent", name: "Custom agent" },
            "custom-agent",
            "freebuff",
          ],
        },
        {},
      ).pipe(
        Effect.provideService(
          ChildProcessSpawner.ChildProcessSpawner,
          ChildProcessSpawner.make(() => Effect.succeed(processHandle({ stdout: "0.2.34" }))),
        ),
      );
      expect(snapshot.models).toMatchObject([{ slug: "freebuff", isCustom: false }]);
    }),
  );

  it.effect.each([
    { reason: "NotFound" as const, installed: false, message: "npm install -g freebuff" },
    { reason: "PermissionDenied" as const, installed: true, message: "Failed to execute" },
  ])("distinguishes a $reason failure from a missing install", ({ reason, installed, message }) =>
    Effect.gen(function* () {
      const snapshot = yield* checkFreebuffProviderStatus(settings, {}).pipe(
        Effect.provideService(
          ChildProcessSpawner.ChildProcessSpawner,
          ChildProcessSpawner.make(() =>
            Effect.fail(
              PlatformError.systemError({
                module: "ChildProcess",
                method: "spawn",
                _tag: reason,
              }),
            ),
          ),
        ),
      );
      expect(snapshot).toMatchObject({
        installed,
        status: "error",
        version: null,
        auth: { status: "unknown" },
      });
      expect(snapshot.message).toContain(message);
    }),
  );

  it.effect("recognizes a missing Windows npm shim", () =>
    Effect.gen(function* () {
      const snapshot = yield* checkFreebuffProviderStatus(settings, {}).pipe(
        Effect.provideService(HostProcess.Platform, "win32"),
        Effect.provideService(
          ChildProcessSpawner.ChildProcessSpawner,
          ChildProcessSpawner.make(() =>
            Effect.succeed(processHandle({ stderr: "freebuff is not recognized", exitCode: 9009 })),
          ),
        ),
      );
      expect(snapshot).toMatchObject({
        installed: false,
        status: "error",
        auth: { status: "unknown" },
      });
      expect(snapshot.message).toContain("npm install -g freebuff");
    }),
  );

  it.effect.each(["0.2.33", "0.1.99", "unknown"])(
    "requires the first verified Freebuff CLI version when the version is %s",
    (version) =>
      Effect.gen(function* () {
        const snapshot = yield* checkFreebuffProviderStatus(settings, {}).pipe(
          Effect.provideService(
            ChildProcessSpawner.ChildProcessSpawner,
            ChildProcessSpawner.make(() => Effect.succeed(processHandle({ stdout: version }))),
          ),
        );
        expect(snapshot).toMatchObject({
          installed: true,
          status: "error",
          auth: { status: "unknown" },
        });
        expect(snapshot.message).toContain(`Freebuff ${MINIMUM_FREEBUFF_VERSION} or newer`);
        expect(snapshot.message).toContain("npm install -g freebuff");
      }),
  );

  it.effect("accepts newer CLI versions", () =>
    Effect.gen(function* () {
      const snapshot = yield* checkFreebuffProviderStatus(settings, {}).pipe(
        Effect.provideService(
          ChildProcessSpawner.ChildProcessSpawner,
          ChildProcessSpawner.make(() => Effect.succeed(processHandle({ stdout: "0.3.0" }))),
        ),
      );
      expect(snapshot).toMatchObject({ installed: true, status: "ready", version: "0.3.0" });
    }),
  );

  it.effect("reports a failed version command without publishing process output", () =>
    Effect.gen(function* () {
      const snapshot = yield* checkFreebuffProviderStatus(settings, {}).pipe(
        Effect.provideService(
          ChildProcessSpawner.ChildProcessSpawner,
          ChildProcessSpawner.make(() =>
            Effect.succeed(processHandle({ stderr: "private diagnostic", exitCode: 1 })),
          ),
        ),
      );
      expect(snapshot).toMatchObject({
        installed: true,
        status: "error",
        auth: { status: "unknown" },
        message: "Freebuff CLI is installed but failed to run.",
      });
      expect(JSON.stringify(snapshot)).not.toContain("private diagnostic");
    }),
  );

  it.effect("interrupts a stalled version probe at the probe deadline", () =>
    Effect.gen(function* () {
      const started = yield* Deferred.make<void>();
      const interrupted = yield* Deferred.make<void>();
      const fiber = yield* checkFreebuffProviderStatus(settings, {}).pipe(
        Effect.provideService(
          ChildProcessSpawner.ChildProcessSpawner,
          ChildProcessSpawner.make(() =>
            Deferred.succeed(started, undefined).pipe(
              Effect.andThen(Effect.never),
              Effect.ensuring(Deferred.succeed(interrupted, undefined)),
            ),
          ),
        ),
        Effect.forkChild,
      );
      yield* Deferred.await(started);
      yield* TestClock.adjust("4 seconds");
      const snapshot = yield* Fiber.join(fiber);
      yield* Deferred.await(interrupted);
      expect(snapshot).toMatchObject({
        installed: true,
        status: "error",
        version: null,
        auth: { status: "unknown" },
        message: "Freebuff CLI is installed but timed out while running `freebuff --version`.",
      });
    }),
  );
});
