// Installing a BMMacintosh release from the page: its zip uploaded to
// POST /api/update, which puts it on the card for the next boot
// (src/webui/update.cpp), then a reboot when wanted.

import * as api from "./api.js";
import { $, fmtBytes } from "./util.js";
import { rebootNow } from "./dashboard.js";

let busy = false;

// A zip that looks like a release (dropped anywhere on the page, drop.js
// asks before sending it here instead of to the shared folder).
export const looksLikeRelease = (name) => /^bmmacintosh.*\.zip$/i.test(name);

function msg(text, err = false) {
  $("upd-msg").textContent = text;
  $("upd-msg").classList.toggle("err", err);
}

export async function installUpdate(file) {
  if (busy) return;
  if (!/\.zip$/i.test(file.name)) {
    msg(file.name + " isn't a zip: a BMMacintosh release comes as one.", true);
    return;
  }
  busy = true;
  $("upd-h").scrollIntoView({ block: "start", behavior: "smooth" });
  $("upd-done").hidden = true;
  $("upd-progress").hidden = false;
  $("upd-name").textContent = file.name;
  $("upd-fill").style.width = "0%";
  msg("Sending it to the Pi…");
  const started = performance.now();
  try {
    const r = await api.installUpdate(file, (sent, total) => {
      const pct = total ? Math.round((sent / total) * 100) : 100;
      $("upd-fill").style.width = pct + "%";
      $("upd-state").textContent = pct < 100
        ? pct + "% · " + fmtBytes(sent) + " of " + fmtBytes(total)
        : "unpacking on the Pi…";
      if (pct >= 100) msg("Unpacking and checking it on the Pi (a few seconds)…");
    });
    $("upd-state").textContent = "installed · " + Math.round((performance.now() - started) / 1000) + " s";
    msg(r.message || "Installed. It runs from the next boot.");
    $("upd-done").hidden = false;
  } catch (e) {
    $("upd-state").textContent = "not installed";
    msg(e.status === 0 || !e.message ? "The upload broke off: nothing was changed. Try again."
      : e.message.replace(/^HTTP \d+ /, ""), true);
  }
  busy = false;
}

export function initUpdate() {
  const zone = $("upd-zone");
  const input = $("upd-input");
  zone.addEventListener("click", () => input.click());
  zone.addEventListener("keydown", (e) => {
    if (e.key === "Enter" || e.key === " ") { e.preventDefault(); input.click(); }
  });
  input.addEventListener("change", () => {
    const f = input.files[0];
    input.value = "";
    if (f) installUpdate(f);
  });
  // Dropped on the box itself: the update, whatever its name (the page's
  // own handler, drop.js, leaves drops here alone).
  zone.addEventListener("dragover", (e) => { e.preventDefault(); e.dataTransfer.dropEffect = "copy"; });
  zone.addEventListener("drop", (e) => {
    e.preventDefault();
    e.stopPropagation();
    document.body.classList.remove("dropping");
    const f = e.dataTransfer.files[0];
    if (f) installUpdate(f);
  });
  $("upd-reboot").addEventListener("click", () => {
    $("upd-done").hidden = true;
    msg("Rebooting into the new version… this page reconnects by itself.");
    rebootNow();
  });
}
