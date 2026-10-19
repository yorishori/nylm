"use strict";

/*
 * nylm's service worker: it keeps the pages on the device, so the app
 * opens without the server (the notes app then works from what it keeps,
 * see public/notes/notes.js). Browsers run it only over HTTPS.
 *
 * Pages, scripts and styles come from the server whenever it answers, and
 * each answer is kept; when it does not answer within NETWORK_MS (or the
 * device is offline), the kept copy is used. The API is never kept here:
 * it always goes to the server.
 */

const SHELL = "nylm-shell";
const NETWORK_MS = 4000;

/* Kept when the worker is installed, so they open offline even if never
 * opened online before. */
const SHELL_FILES = [
  "/", "/common.js", "/home.js", "/style.css", "/favicon.svg", "/manifest.json",
  "/icon-192.png", "/icon-512.png",
  "/notes/", "/notes/notes.js", "/notes/notes.css",
];

self.addEventListener("install", (event) => {
  event.waitUntil(caches.open(SHELL).then((cache) => cache.addAll(SHELL_FILES))
    .then(() => self.skipWaiting()));
});

self.addEventListener("activate", (event) => {
  event.waitUntil(self.clients.claim());
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

self.addEventListener("fetch", (event) => {
  const url = new URL(event.request.url);
  if (event.request.method !== "GET" || url.origin !== self.location.origin ||
      url.pathname.startsWith("/api/")) return;
  event.respondWith(networkFirst(event.request));
});
