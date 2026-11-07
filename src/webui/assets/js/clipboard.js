// The Clipboard card (the Mac II's shared clipboard, src/port/ii/clip_ii.cpp):
// text copied on the Mac shows here; text sent from here is what the Mac
// pastes next. The Mac Plus has none, and the card stays hidden.

import { $ } from "./util.js";
import * as api from "./api.js";

let edited = false;  // typed into since the last refresh: not overwritten

function setMsg(text, err) {
  $("clip-msg").className = "msg" + (err ? " err" : "");
  $("clip-msg").textContent = text || "";
}

async function refresh({ force = false } = {}) {
  let c;
  try {
    c = await api.getClipboard();
  } catch (e) {
    return;
  }
  $("clip-card").hidden = !c.available;
  $("menu-clip").hidden = !c.available;
  if (!c.available) return;
  if (edited && !force) return;
  $("clip-text").value = c.text;
  edited = false;
  $("clip-from").textContent = !c.text ? "Empty"
    : c.from === "mac" ? "Copied on the Mac" : "Sent from a web page";
}

async function send() {
  const text = $("clip-text").value;
  if (new TextEncoder().encode(text).length > 64 * 1024) {
    setMsg("That's more than 64 KB: the Mac's clipboard takes less.", true);
    return;
  }
  $("clip-send").disabled = true;
  try {
    await api.setClipboard(text);
    edited = false;
    $("clip-from").textContent = "Sent from a web page";
    setMsg("Sent: paste on the Mac (Command-V). Characters the Mac hasn't become “?”.");
  } catch (e) {
    setMsg("Couldn't send it — " + e.message, true);
  }
  $("clip-send").disabled = false;
}

async function copy() {
  const box = $("clip-text");
  try {
    await navigator.clipboard.writeText(box.value);
    setMsg("Copied to this computer's clipboard.");
  } catch (e) {
    // Only over HTTPS: select it for Ctrl-C / Command-C instead.
    box.focus();
    box.select();
    setMsg("Selected: press Ctrl-C (Command-C on a Mac) to copy it.");
  }
}

export function initClipboard() {
  $("clip-text").addEventListener("input", () => { edited = true; });
  $("clip-refresh").addEventListener("click", () => { setMsg(""); refresh({ force: true }); });
  $("clip-copy").addEventListener("click", copy);
  $("clip-send").addEventListener("click", send);
  refresh({ force: true });
  // What the Mac copies shows up without a reload (unless you're typing).
  setInterval(() => { if (!document.hidden) refresh(); }, 5000);
}
