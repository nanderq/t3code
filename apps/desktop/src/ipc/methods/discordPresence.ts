import {
  DesktopDiscordPresenceSchema,
  DesktopDiscordPresenceStatusSchema,
} from "@t3tools/contracts";
import * as Effect from "effect/Effect";
import * as Schema from "effect/Schema";

import * as DesktopDiscordPresence from "../../integrations/DesktopDiscordPresence.ts";
import * as IpcChannels from "../channels.ts";
import * as DesktopIpc from "../DesktopIpc.ts";

export const setDiscordPresence = DesktopIpc.makeIpcMethod({
  channel: IpcChannels.SET_DISCORD_PRESENCE_CHANNEL,
  payload: Schema.NullOr(DesktopDiscordPresenceSchema),
  result: Schema.Void,
  handler: Effect.fn("desktop.ipc.discordPresence.set")(function* (presence) {
    const service = yield* DesktopDiscordPresence.DesktopDiscordPresence;
    yield* service.set(presence);
  }),
});

export const getDiscordPresenceStatus = DesktopIpc.makeIpcMethod({
  channel: IpcChannels.GET_DISCORD_PRESENCE_STATUS_CHANNEL,
  payload: Schema.Void,
  result: DesktopDiscordPresenceStatusSchema,
  handler: Effect.fn("desktop.ipc.discordPresence.getStatus")(function* () {
    const service = yield* DesktopDiscordPresence.DesktopDiscordPresence;
    return yield* service.getStatus();
  }),
});
