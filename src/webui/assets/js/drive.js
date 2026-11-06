// The network drive card (the SD card over WebDAV, src/webui/webdav.cpp):
// its address as this browser reached the Pi, how to connect from each
// system, and the settings files, which open in the editor.

import { $ } from "./util.js";
import * as api from "./api.js";
import { openEditor } from "./editor.js";

// The address this page came from is one the computer reaches the Pi at.
const host = location.host || "bmmac";
const httpUrl = "http://" + host + "/sd/";
const davUrl = "dav://" + host + "/sd/";

// The files BMMacintosh starts from, in the card's root.
const SETTINGS = [
  { name: "config.txt", what: "The Pi's firmware: which kernel starts, the display" },
  { name: "cmdline.txt", what: "BMMacintosh's options" },
  { name: "bmmac.cfg", what: "What the menu saves (and the Mac Plus's settings)" },
  { name: "macii.prefs", what: "The Mac II's ROM, disks, memory and screen" },
  { name: "wpa_supplicant.conf", what: "The Wi-Fi network and its password" },
];

function guessOs() {
  const ua = navigator.userAgent;
  if (/Windows/i.test(ua)) return "win";
  if (/Macintosh|Mac OS X/i.test(ua) && !/iPhone|iPad/i.test(ua)) return "mac";
  if (/Linux|X11/i.test(ua) && !/Android/i.test(ua)) return "linux";
  return "win";
}

function showOs(os) {
  document.querySelectorAll(".tab").forEach((t) => {
    const on = t.dataset.os === os;
    t.classList.toggle("on", on);
    t.setAttribute("aria-selected", on ? "true" : "false");
  });
  document.querySelectorAll(".steps").forEach((s) => {
    s.hidden = s.dataset.os !== os;
  });
}

async function copyUrl() {
  const button = $("drive-copy");
  try {
    await navigator.clipboard.writeText(httpUrl);
    button.textContent = "Copied";
  } catch (e) {
    // Not over HTTPS or not allowed: select it instead.
    const range = document.createRange();
    range.selectNodeContents($("drive-url"));
    const sel = window.getSelection();
    sel.removeAllRanges();
    sel.addRange(range);
    button.textContent = "Selected";
  }
  setTimeout(() => { button.textContent = "Copy"; }, 2000);
}

let volume = "";

function renderSettings() {
  const list = $("settings");
  list.textContent = "";
  for (const f of SETTINGS) {
    const li = document.createElement("li");
    const b = document.createElement("button");
    b.type = "button";
    b.className = "file";
    const n = document.createElement("span");
    n.className = "file-name";
    n.textContent = f.name;
    const w = document.createElement("span");
    w.className = "file-what";
    w.textContent = f.what;
    b.append(n, w);
    b.addEventListener("click", () =>
      openEditor({ vol: volume, path: "/" + f.name, name: f.name }));
    li.appendChild(b);
    list.appendChild(li);
  }
}

export async function initDrive() {
  $("drive-url").textContent = httpUrl;
  document.querySelectorAll(".drive-url").forEach((e) => { e.textContent = httpUrl; });
  document.querySelectorAll(".drive-dav").forEach((e) => { e.textContent = davUrl; });
  $("drive-copy").addEventListener("click", copyUrl);
  document.querySelectorAll(".tab").forEach((t) =>
    t.addEventListener("click", () => showOs(t.dataset.os)));
  showOs(guessOs());
  renderSettings();
  try {
    const v = (await api.getVolumes()).volumes || [];
    if (v[0]) volume = v[0].id;
  } catch (e) {
    /* the device's own volume is the default */
  }
}
