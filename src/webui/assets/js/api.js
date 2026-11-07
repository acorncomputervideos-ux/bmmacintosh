// Every call to the device's /api/* endpoints lives here, so the other
// modules deal only in plain values and ApiError.

export class ApiError extends Error {
  constructor(status, detail) {
    super(status === 401
      ? "authentication required — reload the page and enter the PIN"
      : "HTTP " + status + (detail ? " " + detail : ""));
    this.status = status;
  }
}

async function check(r) {
  if (!r.ok) throw new ApiError(r.status, (await r.text()).trim());
  return r;
}

async function getJson(url) {
  return (await check(await fetch(url, { cache: "no-store" }))).json();
}

async function post(url, init) {
  await check(await fetch(url, { method: "POST", ...init }));
}

const fsQuery = (vol, path) =>
  "vol=" + encodeURIComponent(vol) + "&path=" + encodeURIComponent(path);

// Required by the device on every call that changes the card's config or
// layout (see webui.cpp); it keeps other sites from triggering them.
const WEB_HEADER = { "X-BMMacintosh-Web": "1" };

// ---- status ----

export const getStatus = () => getJson("/api/status");
export const getVolumes = () => getJson("/api/volumes");

export const reboot = () => post("/api/reboot", { headers: WEB_HEADER });
export const shutdown = () => post("/api/shutdown", { headers: WEB_HEADER });
export const hardReset = () => post("/api/reset");
export const disableWebUi = () => post("/api/webui/disable");

// ---- the Mac's disks (webui_disks.cpp) ----

export const getDisks = () => getJson("/api/disks");

// op: "start", "insert", "add" (a path in /disks) or "remove" (an index).
// The Mac does it between frames: getDisks()'s "done" goes up when it has.
export async function diskRequest(op, { path, index } = {}) {
  let q = "op=" + op;
  if (path != null) q += "&path=" + encodeURIComponent(path);
  if (index != null) q += "&index=" + index;
  const r = await fetch("/api/disks?" + q, { method: "POST", headers: WEB_HEADER });
  const body = await r.json().catch(() => ({}));
  if (!r.ok) throw new ApiError(r.status, body.message || "");
  return body;
}

// A disk image the Mac isn't using, deleted from the card: { message }.
export async function deleteDisk(path) {
  const r = await fetch("/api/disks?op=delete&path=" + encodeURIComponent(path),
    { method: "POST", headers: WEB_HEADER });
  const body = await r.json().catch(() => ({}));
  if (!r.ok) throw new ApiError(r.status, body.message || "");
  return body;
}

// A dropped file to the card: to "shared" or "disks", path relative
// ("Marathon/Map"). onProgress(sent, total). Resolves { size }; rejects
// with an ApiError (409: there is one of that name).
export function dropFile(file, to, path, replace, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    const q = "to=" + to + "&path=" + encodeURIComponent(path) + (replace ? "&overwrite=1" : "");
    xhr.open("POST", "/api/drop?" + q);
    xhr.setRequestHeader("X-BMMacintosh-Web", "1");
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.upload.onprogress = (e) => onProgress && onProgress(e.loaded, e.total || file.size);
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        try { resolve(JSON.parse(xhr.responseText)); } catch { resolve({}); }
      } else {
        reject(new ApiError(xhr.status, (xhr.responseText || "").trim()));
      }
    };
    xhr.onerror = () => reject(new ApiError(0, "the connection to the Pi broke"));
    xhr.ontimeout = () => reject(new ApiError(0, "the Pi stopped answering"));
    xhr.send(file);
  });
}

// ---- the Mac II's shared clipboard ----

export const getClipboard = () => getJson("/api/clipboard");
export const setClipboard = (text) =>
  post("/api/clipboard", {
    headers: { ...WEB_HEADER, "Content-Type": "text/plain; charset=utf-8" },
    body: text,
  });

// An empty disk image in the disks folder: { path, message }.
export async function newDisk(name, kind, size) {
  const q = "op=new&name=" + encodeURIComponent(name) + "&kind=" + kind + "&size=" + size;
  const r = await fetch("/api/disks?" + q, { method: "POST", headers: WEB_HEADER });
  const body = await r.json().catch(() => ({}));
  if (!r.ok) throw new ApiError(r.status, body.message || "");
  return body;
}

// ---- the settings files (the editor; other files are the network drive's) ----

const downloadUrl = (vol, path) => "/api/fs/download?" + fsQuery(vol, path);

// The file's bytes, as an ArrayBuffer.
export async function readFile(vol, path) {
  const r = await check(await fetch(downloadUrl(vol, path), { cache: "no-store" }));
  return r.arrayBuffer();
}

// Replace an editable config file with `text`.
export const saveFile = (vol, path, text) =>
  post("/api/fs/save?" + fsQuery(vol, path), {
    headers: WEB_HEADER,
    body: text,
  });
