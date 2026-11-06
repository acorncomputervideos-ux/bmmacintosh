// Entry point: wires the modules together.
//
//   api.js        every /api/* call
//   util.js       DOM lookup and formatting
//   machine.js    the running machine
//   dashboard.js  status polling, the card's space, restart / reboot / shut down
//   drive.js      the SD card as a network drive, and the settings files
//   editor.js     overlay editor for the settings files

import { initDashboard, refreshVolumes, startStatusPolling } from "./dashboard.js";
import { initDrive } from "./drive.js";
import { initEditor } from "./editor.js";
import { loadMachine } from "./machine.js";

initDashboard();
initEditor();
await Promise.all([loadMachine(), initDrive()]);
refreshVolumes();
startStatusPolling();
