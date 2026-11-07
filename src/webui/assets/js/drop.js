// Files and folders dropped on the page (or chosen), copied to the card:
// disk images to the disks folder, the rest to shared (the Mac II's
// SD Card disk). One at a time, each with its progress (POST /api/drop).

import * as api from "./api.js";
import { $, fmtBytes } from "./util.js";
import { refresh as refreshDisks } from "./disks.js";

const IMAGE_EXT = /\.(dsk|img|image|hda|hfv|dc42|diskcopy|iso|toast|cdr)$/i;
// What macOS leaves next to files: of no use to the Mac II.
const JUNK = (name) => name === ".DS_Store" || name.startsWith("._") || name === "Thumbs.db";

let busy = false;
const queue = [];

function setMsg(text, err = false) {
  const m = $("drop-msg");
  m.textContent = text;
  m.classList.toggle("err", err);
}

// A folder's files, with their paths in it ("Marathon/Map").
function readFolder(dirEntry, prefix, out) {
  return new Promise((resolve) => {
    const reader = dirEntry.createReader();
    const all = [];
    const more = () => reader.readEntries(async (entries) => {
      if (!entries.length) {
        for (const e of all) await readEntry(e, prefix, out);
        resolve();
        return;
      }
      all.push(...entries);
      more();
    }, () => resolve());
    more();
  });
}

function readEntry(entry, prefix, out) {
  if (entry.isFile) {
    return new Promise((resolve) => entry.file((f) => {
      if (!JUNK(f.name)) out.push({ file: f, path: prefix + f.name });
      resolve();
    }, () => resolve()));
  }
  if (entry.isDirectory) return readFolder(entry, prefix + entry.name + "/", out);
  return Promise.resolve();
}

// What was dropped: files, and folders with what's in them. Each top-level
// item decides where it goes.
async function gather(dataTransfer) {
  const items = [...(dataTransfer.items || [])];
  const entries = items.map((i) => i.webkitGetAsEntry && i.webkitGetAsEntry()).filter(Boolean);
  const groups = [];
  if (entries.length) {
    for (const e of entries) {
      const out = [];
      await readEntry(e, "", out);
      groups.push({ folder: e.isDirectory, name: e.name, files: out });
    }
  } else {
    for (const f of dataTransfer.files || []) {
      if (!JUNK(f.name)) groups.push({ folder: false, name: f.name, files: [{ file: f, path: f.name }] });
    }
  }
  return groups;
}

function destination(group) {
  const chosen = $("drop-to").value;
  if (chosen !== "auto") return chosen;
  return !group.folder && IMAGE_EXT.test(group.name) ? "disks" : "shared";
}

function addRow(job) {
  const li = document.createElement("li");
  const name = document.createElement("span");
  name.className = "drop-name";
  name.textContent = job.to + "/" + job.path;
  const bar = document.createElement("span");
  bar.className = "drop-bar";
  const fill = document.createElement("span");
  bar.append(fill);
  const state = document.createElement("span");
  state.className = "drop-state";
  state.textContent = "waiting · " + fmtBytes(job.file.size);
  li.append(name, bar, state);
  $("drop-list").append(li);
  job.fill = fill;
  job.state = state;
  job.li = li;
}

async function enqueue(groups) {
  let n = 0;
  for (const g of groups) {
    const to = destination(g);
    for (const f of g.files) {
      const job = { file: f.file, path: f.path, to };
      addRow(job);
      queue.push(job);
      n++;
    }
  }
  if (!n) {
    setMsg("Nothing to copy (empty folders, or only macOS's own hidden files).", true);
    return;
  }
  if (!busy) run();
}

async function send(job, replace) {
  const started = performance.now();
  await api.dropFile(job.file, job.to, job.path, replace, (sent, total) => {
    const pct = total ? Math.round((sent / total) * 100) : 100;
    job.fill.style.width = pct + "%";
    const secs = (performance.now() - started) / 1000;
    job.state.textContent = pct + "% · " + fmtBytes(sent) + " of " + fmtBytes(total) +
      (secs > 1 ? " · " + fmtBytes(sent / secs) + "/s" : "");
  });
  return (performance.now() - started) / 1000;
}

async function run() {
  busy = true;
  let done = 0, failed = 0, bytes = 0, toDisks = false;
  const t0 = performance.now();
  setMsg("Copying… The Mac pauses while a big file comes in.");
  while (queue.length) {
    const job = queue.shift();
    job.li.scrollIntoView({ block: "nearest" });
    try {
      let secs;
      try {
        secs = await send(job, $("drop-replace").checked);
      } catch (e) {
        if (e.status !== 409) throw e;
        if (!confirm(job.to + "/" + job.path + " is already on the card. Replace it?")) {
          job.state.textContent = "skipped: already there";
          job.li.classList.add("skipped");
          continue;
        }
        secs = await send(job, true);
      }
      job.fill.style.width = "100%";
      job.state.textContent = "copied · " + fmtBytes(job.file.size) +
        (secs > 1 ? " in " + Math.round(secs) + " s" : "");
      job.li.classList.add("done");
      done++;
      bytes += job.file.size;
      if (job.to === "disks") toDisks = true;
    } catch (e) {
      failed++;
      job.state.textContent = "failed: " + e.message.replace(/^HTTP \d+ /, "");
      job.li.classList.add("failed");
    }
  }
  busy = false;
  const secs = Math.round((performance.now() - t0) / 1000);
  setMsg(done + (done === 1 ? " file" : " files") + " copied (" + fmtBytes(bytes) + ", " + secs + " s)" +
    (failed ? "; " + failed + " failed." : "."), failed > 0);
  if (toDisks) refreshDisks();
}

export function initDrop() {
  const zone = $("drop-zone");
  const input = $("drop-input");
  zone.addEventListener("click", () => input.click());
  zone.addEventListener("keydown", (e) => {
    if (e.key === "Enter" || e.key === " ") { e.preventDefault(); input.click(); }
  });
  input.addEventListener("change", () => {
    const groups = [...input.files].filter((f) => !JUNK(f.name))
      .map((f) => ({ folder: false, name: f.name, files: [{ file: f, path: f.name }] }));
    input.value = "";
    enqueue(groups);
  });

  // Anywhere on the page.
  let depth = 0;
  const hasFiles = (e) => [...(e.dataTransfer?.types || [])].includes("Files");
  document.addEventListener("dragenter", (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    if (depth++ === 0) document.body.classList.add("dropping");
  });
  document.addEventListener("dragleave", (e) => {
    if (!hasFiles(e)) return;
    if (--depth <= 0) { depth = 0; document.body.classList.remove("dropping"); }
  });
  document.addEventListener("dragover", (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = "copy";
  });
  document.addEventListener("drop", async (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    depth = 0;
    document.body.classList.remove("dropping");
    $("drop-h").scrollIntoView({ block: "start", behavior: "smooth" });
    enqueue(await gather(e.dataTransfer));
  });
  window.addEventListener("beforeunload", (e) => {
    if (busy) { e.preventDefault(); e.returnValue = ""; }
  });
}
