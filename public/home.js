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

const APPS = [
  { name: "Plants", href: "/plants/", summary: plantsSummary },
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
