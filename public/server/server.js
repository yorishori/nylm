"use strict";

/*
 * Server app (/server/). Sections by URL hash:
 *   #/system   the host (uptime, load, memory, temperatures, whether a
 *              reboot is needed), its disks, and the latest actions
 *
 * Reads never ask for anything; every button that changes the machine
 * asks for the password again, and the server records it (latest actions).
 */

const TABS = [["system", "System"]];

/* What each recorded action was, by its name in the audit log. */
const ACTION_TEXT = {};

function shell(active, ...content) {
  return el("section", { class: "page" },
    el("header", { class: "app-head" },
      el("h1", {}, "Server"),
      el("nav", { class: "tabs", "aria-label": "Server sections" },
        TABS.map(([key, label]) =>
          el("a", { href: `#/${key}`, "aria-current": key === active ? "page" : null }, label)))),
    ...content);
}

/* ---- helpers ------------------------------------------------------------- */

/* Colour of a fraction full: rose from 90 %, peach from 80 %. */
function fullness(fraction) {
  return fraction >= 0.9 ? "bad" : fraction >= 0.8 ? "warn" : "";
}

/* A bar filled to fraction (0..1), coloured by how full it is. */
function meter(fraction, label) {
  const f = Math.min(1, Math.max(0, fraction || 0));
  const fill = el("span");
  fill.style.width = `${(f * 100).toFixed(1)}%`;
  return el("div", { class: `meter ${fullness(f)}`, role: "img", "aria-label": label }, fill);
}

function percent(fraction) {
  return `${Math.round(fraction * 100)} %`;
}

/* A table with a header row; cols: [label, class]. */
function table(cols, rows) {
  return el("div", { class: "grid-wrap" },
    el("table", { class: "grid" },
      el("thead", {}, el("tr", {}, cols.map(([label, cls]) =>
        el("th", { scope: "col", class: cls || null }, label)))),
      el("tbody", {}, rows)));
}

/* A definition list from [term, value] pairs (null pairs are left out). */
function facts(pairs) {
  return el("dl", { class: "facts" },
    pairs.filter(Boolean).flatMap(([term, value]) => [el("dt", {}, term), el("dd", {}, value)]));
}

/* ---- system -------------------------------------------------------------- */

function hostCard(h) {
  const mem = h.memory;
  const used = mem.total - mem.available;
  const swapUsed = mem.swap_total - mem.swap_free;
  return el("section", { class: "card stack" },
    el("h2", {}, h.hostname),
    facts([
      ["Kernel", h.kernel],
      ["Up", showDuration(h.uptime)],
      ["Load", [`${h.load.map((l) => l.toFixed(2)).join(" · ")} `,
                el("span", { class: "muted" }, `on ${plural(h.cpus, "CPU", "CPUs")}`)]],
      ["Memory", [`${showSize(used)} of ${showSize(mem.total)} used`,
                  meter(used / mem.total, `memory ${percent(used / mem.total)} used`)]],
      mem.swap_total ? ["Swap", [`${showSize(swapUsed)} of ${showSize(mem.swap_total)} used`,
                                 meter(swapUsed / mem.swap_total, "swap used")]] : null,
    ]),
    h.reboot_needed
      ? el("p", { class: "warn" }, "A newer kernel is installed: reboot to use it (Updates).")
      : null);
}

function tempClass(c) {
  return c >= 90 ? "bad" : c >= 80 ? "warn" : "";
}

function temperaturesCard(temps) {
  return el("section", { class: "card stack" },
    el("h2", {}, "Temperatures"),
    temps.length
      ? table([["Sensor"], ["", "num"]], temps.map((t) => el("tr", {},
          el("td", {}, t.label ? `${t.sensor} · ${t.label}` : t.sensor),
          el("td", { class: `num ${tempClass(t.celsius)}` }, `${t.celsius.toFixed(1)} °C`))))
      : el("p", { class: "empty" }, "No temperature sensors found."));
}

function mountRow(m) {
  if (m.size == null) {
    return el("tr", {}, el("td", { class: "path" }, m.path), el("td", { class: "muted" }, m.device),
      el("td", { colspan: 3, class: "bad" }, "can not be read"));
  }
  const f = m.size ? m.used / m.size : 0;
  return el("tr", {},
    el("td", { class: "path" }, m.path),
    el("td", { class: "muted small" }, `${m.device} · ${m.type}`),
    el("td", { class: "num" }, showSize(m.size)),
    el("td", { class: `num ${fullness(f)}` }, `${showSize(m.free)} free`),
    el("td", { class: "col-meter" }, meter(f, `${percent(f)} used`)));
}

function disksCard(mounts) {
  return el("section", { class: "card stack" },
    el("h2", {}, "Disks"),
    table([["Mounted on"], ["Device"], ["Size", "num"], ["Free", "num"], ["Used"]],
          mounts.map(mountRow)));
}

function actionItem(a) {
  const failed = !a.result.startsWith("ok") && a.result !== "started";
  return el("li", { class: "card stack" },
    el("header", { class: "line" },
      el("strong", {}, ACTION_TEXT[a.action] || a.action),
      el("span", { class: "muted" }, showTime(a.at))),
    a.detail ? el("p", { class: "path" }, a.detail) : null,
    el("p", { class: failed ? "bad" : "muted" }, `${a.result} · from ${a.client}`));
}

function actionsSection(actions) {
  return el("section", { class: "section" },
    el("header", {}, el("h2", {}, "Latest actions ", el("span", { class: "count" }, actions.length))),
    actions.length
      ? el("ul", { class: "list" }, actions.map(actionItem))
      : el("p", { class: "empty" }, "Nothing was started from here yet."));
}

async function systemPage() {
  const [h, audit] = await Promise.all([api("GET", "/api/server"),
                                         api("GET", "/api/server/audit")]);
  return shell("system",
    el("div", { class: "columns" }, hostCard(h), temperaturesCard(h.temperatures)),
    disksCard(h.mounts),
    actionsSection(audit.actions));
}

/* ---- routing ------------------------------------------------------------- */

function route(parts) {
  const [section, rest] = parts;
  if (section === "system" && rest === undefined) return systemPage();
  go("#/system");
  return null;
}

startApp(route);
