//
// webui_fs.h
//
// Filesystem endpoints for the web UI: directory listing and download
// (read-only) plus a single streamed upload path. All access is confined
// to the configured SD card volume and sandboxed against path traversal;
// upload additionally refuses a small denylist of config/firmware files.
// FatFs calls are serialised with the emulator by FF_FS_REENTRANT and
// chunked so a transfer never holds the volume lock for long.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _webui_fs_h
#define _webui_fs_h

class CSocket;

// GET /api/volumes  -> JSON list of browsable volumes with size/free.
void WebUiFsVolumes(CSocket *socket);

// GET /api/fs/list?vol=SD&path=/dir  -> JSON directory listing.
void WebUiFsList(CSocket *socket, const char *query);

// GET /api/fs/download?vol=SD&path=/dir/file  -> raw file bytes.
void WebUiFsDownload(CSocket *socket, const char *query);

// POST /api/fs/upload?vol=SD&path=/dir/file  <- the raw request body is
// the file. prefetched[0..prefetched_len) are body bytes already read
// with the request headers; the remainder (up to content_length total)
// is streamed from the socket. content_length < 0 means no
// Content-Length header was sent (rejected with 411). An optional
// &mtime=YYYY-MM-DDTHH:MM:SS (local time) query parameter sets the
// file's modified time; without it the file gets the current FAT time.
void WebUiFsUpload(CSocket *socket, const char *query,
                   const unsigned char *prefetched, unsigned prefetched_len,
                   long content_length);

// POST /api/drop?to=shared|disks&path=Folder/Name  <- the raw request body
// is a file dropped on the web page, for the card's shared folder (the Mac
// II's SD Card disk) or disks folder. path is relative (UTF-8; folders in
// it are made), and &overwrite=1 replaces a file of that name (409
// otherwise). The Mac's old Finder info and resource fork for it go.
void WebUiFsDrop(CSocket *socket, const char *query,
                 const unsigned char *prefetched, unsigned prefetched_len,
                 long content_length);

// GET /api/get?path=/disks/Mac%20OS.hda  -> a file on the card, to save
// (the path in UTF-8, as the listings give it). Answered alongside the
// page's other requests; the Mac pauses meanwhile (the transfer window),
// and saves its disks' changes first, so a disk image in use downloads
// as the Mac left it.
void WebUiFsGet(CSocket *socket, const char *query);

// POST /api/update  <- the raw request body is a BMMacintosh release zip,
// installed on the card for the next boot (update.h). Answers JSON:
// {"ok":..., "replaced":n, "kept":n, "version":"...", "message":"..."}.
void WebUiFsUpdate(CSocket *socket, const unsigned char *prefetched, unsigned prefetched_len,
                   long content_length);

// POST /api/fs/save?vol=SD&path=/settings.txt  <- the raw request body
// replaces one of the editable config files (cmdline.txt, config.txt,
// wpa_supplicant.conf, macii.prefs, bmmac.cfg): up to 256 KB of
// text, no NUL bytes. Written to "<name>.part", with the previous version
// kept as "<name>.bak". Arguments are as for WebUiFsUpload. GET /api/fs/list
// marks the editable entries with "edit":true.
void WebUiFsSave(CSocket *socket, const char *query,
                 const unsigned char *prefetched, unsigned prefetched_len,
                 long content_length);

// POST /api/fs/delete?vol=SD&path=/dir/file  -> remove a file (or an
// empty directory). The same protected paths as upload are refused.
// With &recursive=1 a directory is removed together with its contents (up
// to 24 levels deep). If something can't be deleted it stops there with 409.
void WebUiFsDelete(CSocket *socket, const char *query);

// POST /api/fs/mkdir?vol=SD&path=/dir/newname  -> create a folder. The
// last path segment is the new name (plain ASCII); its parent must exist.
// Refused (403) inside /firmware or under a protected name, and 409 when
// something of that name already exists.
void WebUiFsMkdir(CSocket *socket, const char *query);

// POST /api/fs/rename?vol=SD&path=/dir/old&to=new  -> rename a file or
// folder within its own folder; `to` is a bare plain-ASCII name. The
// protected paths are refused as the source or the new name, and 409 is
// answered if the new name is taken (a change of case only is allowed).
void WebUiFsRename(CSocket *socket, const char *query);


#endif
