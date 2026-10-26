import { TextGenerationError } from "@t3tools/contracts";
import * as Effect from "effect/Effect";
import * as TextGenerationOperations from "@t3tools/provider-core/server/textGenerationOperations";
import type { FreebuffSettings } from "../settings.ts";

export const makeFreebuffTextGeneration = Effect.fn("FreebuffTextGeneration.make")(function (
  _settings: FreebuffSettings,
  _environment: NodeJS.ProcessEnv,
) {
  // Freebuff's native session can run tools; background titles and git writing must not start one.
  return Effect.succeed(
    TextGenerationOperations.fromRunner("FreebuffTextGeneration", (request) =>
      Effect.fail(
        new TextGenerationError({
          operation: request.operation,
          detail:
            "Freebuff does not provide tool-free background text generation. Choose another model for titles and source control writing.",
        }),
      ),
    ),
  );
});
