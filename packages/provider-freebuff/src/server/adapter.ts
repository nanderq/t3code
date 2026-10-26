import { ProviderDriverKind, type OrchestrationV2ProviderCapabilities } from "@t3tools/contracts";
import * as Effect from "effect/Effect";
import * as Path from "effect/Path";
import { AcpProviderCapabilitiesV2, makeAcpAdapterV2 } from "@t3tools/provider-acp/server/adapter";
import * as ProviderHost from "@t3tools/provider-core/server/ProviderHost";
import { resolveSelfInvocation } from "@t3tools/shared/nodeRuntime";
import * as AcpErrors from "effect-acp/errors";
import { makeFreebuffRuntime } from "./runtime.ts";
import type { FreebuffSettings } from "../settings.ts";

const DRIVER = ProviderDriverKind.make("freebuff");
const CAPABILITIES = {
  ...AcpProviderCapabilitiesV2,
  streaming: { ...AcpProviderCapabilitiesV2.streaming, streamsReasoning: false },
  approvals: {
    ...AcpProviderCapabilitiesV2.approvals,
    supportsCommandApproval: false,
    supportsFileReadApproval: false,
    supportsFileChangeApproval: false,
  },
  planning: {
    ...AcpProviderCapabilitiesV2.planning,
    emitsPlanUpdated: false,
    emitsTodoList: false,
    supportsStructuredQuestions: false,
  },
} satisfies OrchestrationV2ProviderCapabilities;

export const makeFreebuffAdapterV2 = Effect.fn("FreebuffAdapter.make")(function* (options: {
  readonly instanceId: Parameters<typeof makeAcpAdapterV2>[0]["instanceId"];
  readonly settings: FreebuffSettings;
  readonly environment: NodeJS.ProcessEnv;
  readonly createSession?: Parameters<typeof makeFreebuffRuntime>[0]["createSession"];
  readonly continuationRequests?: Parameters<typeof makeAcpAdapterV2>[0]["continuationRequests"];
  readonly nativeLogging?: Parameters<typeof makeAcpAdapterV2>[0]["nativeLogging"];
}) {
  const host = yield* ProviderHost.ProviderHost;
  const path = yield* Path.Path;
  return yield* makeAcpAdapterV2({
    instanceId: options.instanceId,
    selfInvocation: yield* resolveSelfInvocation(),
    ...(options.continuationRequests ? { continuationRequests: options.continuationRequests } : {}),
    ...(options.nativeLogging ? { nativeLogging: options.nativeLogging } : {}),
    flavor: {
      driver: DRIVER,
      runtimeHarness: "Freebuff",
      capabilities: CAPABILITIES,
      negotiateCapabilities: false,
      makeRuntime: (input) =>
        input.runtimePolicy.runtimeMode !== "full-access" ||
        input.runtimePolicy.approvalPolicy !== undefined ||
        input.runtimePolicy.sandboxPolicy !== undefined
          ? Effect.fail(
              AcpErrors.AcpRequestError.invalidParams(
                "Freebuff supports Full access only. It cannot enforce custom approval or sandbox policies.",
              ),
            )
          : makeFreebuffRuntime({
              binaryPath: options.settings.binaryPath,
              cwd: input.cwd,
              environment: { ...options.environment, ...input.processEnvironment },
              stateDirectory: path.join(
                host.paths.stateDir,
                "providers",
                "freebuff",
                encodeURIComponent(options.instanceId),
              ),
              ...(input.resumeSessionId ? { resumeSessionId: input.resumeSessionId } : {}),
              ...(options.createSession ? { createSession: options.createSession } : {}),
            }),
    },
  });
});
