"use strict";

/* nylm home page: one tile per app, each with a short live summary. */

/* Pills summing up the plants due list: late, today, this week. */
async function plantsSummary() {
  const due = await api("GET", "/api/plants/due");
  const late = due.filter((d) => d.days_left < 0).length;
  const now = due.filter((d) => d.days_left === 0).length;
  const week = due.filter((d) => d.days_left > 0 && d.days_left <= 7).length;
  if (late + now + week === 0) {
    return [el("span", { class: "pill" }, "Nothing due this week")];
  }
  return [
    late ? el("span", { class: "pill late" }, `${late} late`) : null,
    now ? el("span", { class: "pill today" }, `${now} today`) : null,
    week ? el("span", { class: "pill" }, `${week} this week`) : null,
  ];
}

/* Pills for the music library: albums, pending changes, a running service. */
async function musicSummary() {
  const o = await api("GET", "/api/music");
  if (!o.configured) return [el("span", { class: "pill" }, "Not set up")];
  return [
    el("span", { class: "pill" }, plural(o.stats.albums, "album", "albums")),
    o.pending ? el("span", { class: "pill" }, `${o.pending} pending`) : null,
    o.busy === "scan" ? el("span", { class: "pill" }, "Scanning") : null,
    o.busy === "write" ? el("span", { class: "pill" }, "Writing") : null,
    o.available ? null : el("span", { class: "pill today" }, "Folder missing"),
  ];
}

/* Pills for the server: what needs attention, else how long it is up. */
async function serverSummary() {
  const h = await api("GET", "/api/server");
  const full = h.mounts.filter((m) => m.size && m.used / m.size >= 0.9);
  const pills = [
    h.reboot_needed ? el("span", { class: "pill today" }, "Reboot needed") : null,
    ...full.map((m) => el("span", { class: "pill late" }, `${m.path} nearly full`)),
  ].filter(Boolean);
  return pills.length ? pills : [el("span", { class: "pill" }, `Up ${showDuration(h.uptime)}`)];
}

const APPS = [
  { name: "Plants", href: "/plants/", summary: plantsSummary },
  { name: "Music", href: "/music/", summary: musicSummary },
  { name: "Server", href: "/server/", summary: serverSummary },
];

async function homePage() {
  const tiles = await Promise.all(APPS.map(async (a) =>
    el("a", { class: "tile", href: a.href },
      el("h2", {}, a.name),
      el("div", { class: "pills" }, await a.summary()))));
  return el("section", {},
    el("h1", {}, "Apps"),
    el("nav", { class: "tiles", "aria-label": "Apps" }, tiles));
}

startApp(homePage, { home: true });
