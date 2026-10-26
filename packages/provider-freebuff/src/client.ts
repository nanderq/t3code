import { ProviderDriverKind } from "@t3tools/contracts";
import { defineProviderClient } from "@t3tools/provider-core/client";

import { FreebuffSettings } from "./settings.ts";

export const freebuffClient = defineProviderClient({
  driverKind: ProviderDriverKind.make("freebuff"),
  label: "Freebuff",
  settingsSchema: FreebuffSettings,
  icon: {
    viewBox: "0 12.74 77.18 77.19",
    fill: { light: "#0F0F0F", dark: "#F5F5F5" },
    paths: [
      { d: "M77.18 12.74V32.04H19.3V89.93H0V12.74Z" },
      { d: "M63.62 37.63V56.93H44.32V89.93H25.03V37.63Z" },
    ],
  },
});
