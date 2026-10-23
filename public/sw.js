"use strict";

/*
 * nylm's service worker: it keeps the pages of the apps that work offline
 * (home, notes, plants) on the device, so they open without the server and
 * work from what they keep (see notes.js, plants.js). Browsers run it only
 * over HTTPS. The other apps (music, server) are online only: their pages
 * are never kept, so offline the browser says it can't reach them.
 *
 * The kept pages (SHELL_FILES) come from the server whenever it answers,
 * and each answer is kept; when it does not answer within NETWORK_MS (or
 * the device is offline), the kept copy is used. The API always goes to the
 * server (the apps keep what they need themselves), except the plants
 * journal's photos: a photo's URL names its bytes (a hash), so once kept
 * it is used as it is. Logging out deletes them (PHOTOS, see common.js).
 */

const SHELL = "nylm-shell";
const PHOTOS = "nylm-photos";
const NETWORK_MS = 4000;

/* The files kept, from the moment the worker is installed: they open
 * offline even if never opened online before. Nothing else is. */
const SHELL_FILES = [
  "/", "/common.js", "/home.js", "/style.css", "/favicon.svg", "/manifest.json",
  "/icon-192.png", "/icon-512.png",
  "/notes/", "/notes/notes.js", "/notes/notes.css",
  "/plants/", "/plants/plants.js", "/plants/plants.css",
];

self.addEventListener("install", (event) => {
  event.waitUntil(caches.open(SHELL).then((cache) => cache.addAll(SHELL_FILES))
    .then(() => self.skipWaiting()));
});

/* Drops what an older worker kept that is no longer in SHELL_FILES. */
async function dropOthers() {
  const cache = await caches.open(SHELL);
  const keep = new Set(SHELL_FILES.map((path) => new URL(path, self.location.origin).href));
  for (const request of await cache.keys()) {
    if (!keep.has(request.url)) await cache.delete(request);
  }
}

self.addEventListener("activate", (event) => {
  event.waitUntil(dropOthers().then(() => self.clients.claim()));
});

/* The server's answer (kept for later), or the kept copy if the server is
 * slow or out of reach; the server's error if there is no copy. */
async function networkFirst(request) {
  const cache = await caches.open(SHELL);
  const network = fetch(request).then((res) => {
    if (res.ok) cache.put(request, res.clone());
    return res;
  });
  network.catch(() => {}); /* a late failure after the copy was used is no error */
  const timeout = new Promise((resolve) => setTimeout(resolve, NETWORK_MS));
  try {
    const res = await Promise.race([network, timeout]);
    if (res) return res;
  } catch (err) {
    /* offline: the copy below */
  }
  const kept = await cache.match(request);
  return kept || network;
}

/* The kept photo, else the server's (kept if it is one). */
async function keptFirst(request) {
  const cache = await caches.open(PHOTOS);
  const kept = await cache.match(request);
  if (kept) return kept;
  const res = await fetch(request);
  if (res.ok) await cache.put(request, res.clone());
  return res;
}

self.addEventListener("fetch", (event) => {
  const url = new URL(event.request.url);
  if (event.request.method !== "GET" || url.origin !== self.location.origin) return;
  if (url.pathname === "/api/plants/photo") event.respondWith(keptFirst(event.request));
  else if (SHELL_FILES.includes(url.pathname)) event.respondWith(networkFirst(event.request));
});
