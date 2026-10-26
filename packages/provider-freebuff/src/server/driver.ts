import { ProviderDriverKind } from "@t3tools/contracts";
import * as HostProcess from "@t3tools/shared/HostProcess";
import * as Effect from "effect/Effect";
import * as Schema from "effect/Schema";
import type * as Scope from "effect/Scope";
import * as ChildProcessSpawner from "effect/process/ChildProcessSpawner";

import * as ProviderHost from "@t3tools/provider-core/server/ProviderHost";
import * as ProviderContinuationRequests from "@t3tools/provider-core/server/ProviderContinuationRequests";
import {
  defaultProviderContinuationIdentity,
  type ProviderDriver,
  type ProviderInstance,
} from "@t3tools/provider-core/server/driver";
import { ProviderDriverError } from "@t3tools/provider-core/server/errors";
import { mergeProviderInstanceEnvironment } from "@t3tools/provider-core/server/instanceEnvironment";
import { withInstanceIdentity } from "@t3tools/provider-core/server/instanceIdentity";
import { makeManualOnlyProviderMaintenanceCapabilities } from "@t3tools/provider-core/server/maintenanceResolver";
import { makeManagedServerProvider } from "@t3tools/provider-core/server/managedProvider";
import { expandHomePath } from "@t3tools/provider-core/server/pathExpansion";
import {
  haveProviderSnapshotSettingsChanged,
  makeProviderSnapshotSettingsSource,
  type ProviderSnapshotSettings,
} from "@t3tools/provider-core/server/snapshotSettings";
import { FreebuffSettings } from "../settings.ts";
import { makeFreebuffAdapterV2 } from "./adapter.ts";
import { buildInitialFreebuffProviderSnapshot, checkFreebuffProviderStatus } from "./status.ts";
import { makeFreebuffTextGeneration } from "./textGeneration.ts";

const DRIVER_KIND = ProviderDriverKind.make("freebuff");
const decodeFreebuffSettings = Schema.decodeSync(FreebuffSettings);
const MAINTENANCE_CAPABILITIES = makeManualOnlyProviderMaintenanceCapabilities({
  provider: DRIVER_KIND,
  packageName: "freebuff",
});

export type FreebuffDriverEnv =
  | Exclude<
      | Effect.Services<ReturnType<typeof makeFreebuffAdapterV2>>
      | Effect.Services<ReturnType<typeof makeFreebuffTextGeneration>>,
      Scope.Scope
    >
  | ProviderHost.ProviderHost
  | ChildProcessSpawner.ChildProcessSpawner;

export const FreebuffDriver: ProviderDriver<FreebuffSettings, FreebuffDriverEnv> = {
  driverKind: DRIVER_KIND,
  metadata: { displayName: "Freebuff", supportsMultipleInstances: true },
  configSchema: FreebuffSettings,
  defaultConfig: () => decodeFreebuffSettings({}),
  create: Effect.fn("FreebuffDriver.create")(function* ({
    instanceId,
    displayName,
    accentColor,
    environment,
    enabled,
    config,
  }) {
    const host = yield* ProviderHost.ProviderHost;
    const spawner = yield* ChildProcessSpawner.ChildProcessSpawner;
    const continuationRequests = yield* ProviderContinuationRequests.ProviderContinuationRequests;
    const hostEnvironment = yield* HostProcess.Environment;
    const processEnvironment = yield* mergeProviderInstanceEnvironment(
      environment,
      hostEnvironment,
    );
    const effectiveConfig = {
      ...config,
      enabled,
      binaryPath: expandHomePath(config.binaryPath, yield* HostProcess.HomeDirectory),
    } satisfies FreebuffSettings;
    const continuationIdentity = defaultProviderContinuationIdentity({
      driverKind: DRIVER_KIND,
      instanceId,
    });
    const stampIdentity = withInstanceIdentity({
      instanceId,
      driverKind: DRIVER_KIND,
      displayName,
      accentColor,
      continuationGroupKey: continuationIdentity.continuationKey,
    });
    const snapshotSettings = yield* makeProviderSnapshotSettingsSource(effectiveConfig);
    const snapshot = yield* makeManagedServerProvider<ProviderSnapshotSettings<FreebuffSettings>>({
      resolveMaintenance: () => Effect.succeed(MAINTENANCE_CAPABILITIES),
      getSettings: snapshotSettings.getSettings,
      streamSettings: snapshotSettings.streamSettings,
      haveSettingsChanged: haveProviderSnapshotSettingsChanged,
      initialSnapshot: (settings) =>
        buildInitialFreebuffProviderSnapshot(settings.provider).pipe(Effect.map(stampIdentity)),
      checkProvider: checkFreebuffProviderStatus(
        effectiveConfig,
        processEnvironment,
        host.paths.cwd,
      ).pipe(
        Effect.map(stampIdentity),
        Effect.provideService(ChildProcessSpawner.ChildProcessSpawner, spawner),
      ),
    }).pipe(
      Effect.mapError(
        (cause) =>
          new ProviderDriverError({
            driver: DRIVER_KIND,
            instanceId,
            detail: "Failed to build Freebuff snapshot.",
            cause,
          }),
      ),
    );
    const orchestrationAdapter = yield* makeFreebuffAdapterV2({
      instanceId,
      settings: effectiveConfig,
      environment: processEnvironment,
      continuationRequests,
    }).pipe(
      Effect.mapError(
        (cause) =>
          new ProviderDriverError({
            driver: DRIVER_KIND,
            instanceId,
            detail: "Failed to build Freebuff orchestration adapter.",
            cause,
          }),
      ),
    );
    const textGeneration = yield* makeFreebuffTextGeneration(
      effectiveConfig,
      processEnvironment,
    ).pipe(
      Effect.mapError(
        (cause) =>
          new ProviderDriverError({
            driver: DRIVER_KIND,
            instanceId,
            detail: "Failed to build Freebuff text generation.",
            cause,
          }),
      ),
    );
    return {
      instanceId,
      driverKind: DRIVER_KIND,
      continuationIdentity,
      displayName,
      accentColor,
      enabled,
      snapshot,
      orchestrationAdapter,
      textGeneration,
    } satisfies ProviderInstance;
  }),
};
