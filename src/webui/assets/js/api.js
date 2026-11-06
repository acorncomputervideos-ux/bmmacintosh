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
