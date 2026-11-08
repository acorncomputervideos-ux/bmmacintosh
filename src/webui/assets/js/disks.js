// The Disks card: the Mac's disks now, the disk images in the card's
// disks folder, and changing them (GET and POST /api/disks,
// src/webui/webui_disks.cpp). The Mac makes a change between its frames;
// the card waits for it ("done" goes up) and shows what happened.

import { $, fmtBytes } from "./util.js";
import * as api from "./api.js";

let state = null;
let busy = false;

const base = (p) => p.slice(p.lastIndexOf("/") + 1);
// The path below the disks folder, for the list.
const shortPath = (p) => p.replace(/^\/disks\//, "");

function setMsg(text, err) {
  const m = $("disks-msg");
  m.className = "msg" + (err ? " err" : "");
  m.textContent = text || "";
}

function button(label, run, { danger = false } = {}) {
  const b = document.createElement("button");
  b.type = "button";
  b.className = "btn btn-small" + (danger ? " btn-danger" : "");
  b.textContent = label;
  b.disabled = busy;
  b.addEventListener("click", run);
  return b;
}

function row(label, value, buttons = []) {
  const li = document.createElement("li");
  const l = document.createElement("span");
  l.className = "slot-label";
  l.textContent = label;
  const v = document.createElement("span");
  v.className = "slot-value" + (value ? "" : " empty");
  v.textContent = value ? base(value) : "empty";
  if (value) v.title = value;
  const acts = document.createElement("span");
  acts.className = "slot-acts";
  acts.append(...buttons);
  li.append(l, v, acts);
  return li;
}

// Whether a Mac can start from an image (it has a System: boot blocks).
function startable(path) {
  const img = state.images.find((i) => i.path === path);
  return !!(img && img.startable);
}

function renderNow() {
  const box = $("disks-now");
  box.textContent = "";
  const list = document.createElement("ul");
  list.className = "slots";
  if (state.machine === "ii") {
    if (!state.hard.length) list.append(row("Hard disk", ""));
    state.hard.forEach((p, i) => {
      const acts = [];
      if (i > 0 && startable(p)) acts.push(button("Start from", () => start(p)));
      acts.push(button("Remove", () => change("remove", { index: i },
        "Remove " + base(p) + " from the Mac's hard disks? (The file stays on the card.)")));
      list.append(row(i === 0 ? "Starts from" : "Hard disk " + (i + 1), p, acts));
    });
    const ejectable = (label, path, index) =>
      list.append(row(label, path, path ? [button("Eject", () => eject(index, path))] : []));
    ejectable("Floppy drive 1", state.floppy[0], 0);
    ejectable("Floppy drive 2", state.floppy[1], 1);
    ejectable("CD-ROM drive", state.cdrom, 2);
    box.append(list);
    const note = document.createElement("p");
    note.className = "note";
    note.textContent = "Hard disk changes take effect when the Mac restarts. " +
      "Floppies and CD-ROMs go in now; Eject asks the Mac to put one away, " +
      "as dragging it to the Trash does.";
    box.append(note);
  } else {
    list.append(row("Disk", state.disk));
    box.append(list);
  }
}

// A disk image to save on this computer (GET /api/get). One the Mac is
// using comes as the Mac left it: it saves its changes first and pauses
// while it goes.
function downloadLink(path, used) {
  const a = document.createElement("a");
  a.className = "btn btn-small";
  a.textContent = "Download";
  a.href = "/api/get?path=" + encodeURIComponent(path);
  a.setAttribute("download", base(path));
  a.title = used
    ? "The Mac saves its changes and pauses while it downloads, so the copy is the disk as it left it."
    : "Save a copy on this computer.";
  return a;
}

const KIND = { floppy: "Floppy", cd: "CD-ROM", hard: "Hard disk", disk: "Disk" };

function renderImages() {
  const ul = $("disks-images");
  ul.textContent = "";
  const images = state.images.slice().sort((a, b) =>
    a.path.localeCompare(b.path, undefined, { numeric: true, sensitivity: "base" }));
  $("disks-note").textContent = images.length
    ? "Start from is offered for disks with a System on them." +
      (state.more ? " The first " + images.length + " are shown." : "")
    : "None yet: copy disk images to the disks folder on the network drive.";
  const inUse = new Set([...state.hard, ...state.floppy, state.cdrom, state.disk,
    ...(state.open || [])].filter(Boolean));
  for (const img of images) {
    const li = document.createElement("li");
    const name = document.createElement("span");
    name.className = "img-name";
    name.textContent = shortPath(img.path);
    const meta = document.createElement("span");
    meta.className = "img-meta";
    meta.textContent = KIND[img.kind] + " · " + fmtBytes(img.size) +
      (inUse.has(img.path) ? " · in use" : "");
    const acts = document.createElement("span");
    acts.className = "img-acts";
    if ((img.kind === "floppy" || img.kind === "cd") && !inUse.has(img.path)) {
      acts.append(button("Insert", () => change("insert", { path: img.path })));
    }
    if (img.kind === "hard" && !state.hard.includes(img.path)) {
      acts.append(button("Add", () => change("add", { path: img.path })));
    }
    if (img.startable) acts.append(button("Start from", () => start(img.path)));
    acts.append(downloadLink(img.path, inUse.has(img.path)));
    if (!inUse.has(img.path)) {
      acts.append(button("Delete", () => deleteDisk(img.path), { danger: true }));
    }
    const text = document.createElement("span");
    text.className = "img-text";
    text.append(name, meta);
    li.append(text, acts);
    ul.append(li);
  }
}

function render() {
  renderNow();
  renderImages();
  $("new-disk").hidden = state.machine !== "ii";
  $("new-make").disabled = busy;
}

async function deleteDisk(path) {
  if (busy) return;
  if (!confirm("Delete " + base(path) + " from the card?\n\nEverything on the disk is lost.")) return;
  busy = true;
  render();
  try {
    setMsg((await api.deleteDisk(path)).message);
  } catch (e) {
    setMsg(e.message.replace(/^HTTP \d+ /, ""), true);
  }
  busy = false;
  await refresh();
}

async function makeDisk(ev) {
  ev.preventDefault();
  if (busy) return;
  const name = $("new-name").value.trim();
  const [kind, size] = $("new-size").value.split(":");
  busy = true;
  render();
  setMsg(kind === "hard" && size > 500 * 1048576
    ? "Making it (a few seconds for a big one)…" : "Making it…");
  let made;
  try {
    made = await api.newDisk(name, kind, size);
  } catch (e) {
    busy = false;
    setMsg(e.message.replace(/^HTTP \d+ /, ""), true);
    await refresh();
    return;
  }
  busy = false;
  setMsg(made.message);
  await refresh();
  if ($("new-use").checked) {
    await change(kind === "floppy" ? "insert" : "add", { path: made.path });
  }
}

// Mac OS ejects it when it is next idle: wait for the drive to empty, or
// for the Mac's answer (a second "done": in use, or too busy).
async function eject(index, path) {
  if (busy) return;
  busy = true;
  render();
  setMsg("Asking the Mac to eject " + base(path) + "…");
  const slot = (s) => (index === 2 ? s.cdrom : s.floppy[index]);
  try {
    const before = state.done;
    await api.diskRequest("eject", { index });
    let seen = -1;
    for (let i = 0; i < 32; i++) {
      await new Promise((r) => setTimeout(r, 250));
      const s = await api.getDisks();
      if (s.pending) continue;
      if (seen < 0 && s.done !== before) {
        seen = s.done;          // queued: "Ejecting … when the Mac is next idle"
        setMsg(s.result);
      }
      if (seen >= 0 && (s.done !== seen || !slot(s))) {
        state = s;
        setMsg(s.result);
        break;
      }
    }
  } catch (e) {
    setMsg(e.status && e.message ? e.message.replace(/^HTTP \d+ /, "") : "Couldn't ask the Mac — " + e.message, true);
  }
  busy = false;
  await refresh();
}

function start(path) {
  change("start", { path },
    "Start the Mac again from " + base(path) + "?\n\nAnything not saved on the Mac is lost.");
}

async function change(op, args, question) {
  if (busy) return;
  if (question && !confirm(question)) return;
  busy = true;
  render();
  setMsg("Asking the Mac…");
  const before = state.done;
  try {
    await api.diskRequest(op, args);
    // The Mac does it between frames: wait until it has.
    for (let i = 0; i < 40; i++) {
      await new Promise((r) => setTimeout(r, 250));
      const s = await api.getDisks();
      if (s.done !== before && !s.pending) {
        state = s;
        setMsg(s.result);
        break;
      }
    }
  } catch (e) {
    setMsg(e.status && e.message ? e.message.replace(/^HTTP \d+ /, "") : "Couldn't ask the Mac — " + e.message, true);
  }
  busy = false;
  await refresh();
}

export async function refresh() {
  try {
    state = await api.getDisks();
    render();
  } catch (e) {
    $("disks-note").textContent = "Couldn't read the disks — " + e.message;
  }
}

export function initDisks() {
  $("new-disk").addEventListener("submit", makeDisk);
  refresh();
}
