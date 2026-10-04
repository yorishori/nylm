"use strict";

/*
 * Server app (/server/). Sections by URL hash:
 *   #/system   the host (uptime, load, memory, temperatures, whether a
 *              reboot is needed), its disks, their SMART health, what takes
 *              the room on them, and the latest actions
 *
 * Reads never ask for anything; every button that changes the machine
 * asks for the password again, and the server records it (latest actions).
 * Long work is a job (a systemd unit): the page shows its state, checks
 * back while it runs, and shows its log.
 */

const TABS = [["system", "System"]];

/* What each recorded action was, by its name in the audit log. */
const ACTION_TEXT = {
  "disk-usage": "Measure disk usage",
};

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

/* ---- jobs and logs ------------------------------------------------------- */

function running(job) {
  return ["active", "activating", "deactivating", "reloading"].includes(job.active);
}

/* One line on a job (a systemd unit): running, how its last run ended,
 * or that it has not run since the server started. */
function jobState(job) {
  if (job.load === "not-found") {
    return el("p", { class: "bad" }, "Not installed: run deploy/install.sh on the server.");
  }
  if (running(job)) {
    return el("p", { class: "warn" }, `Running since ${showTime(job.started)}…`);
  }
  if (!job.started) return el("p", { class: "muted" }, "Not run since the server started.");
  if (job.result === "success") {
    return el("p", { class: "ok" }, `Done ${showTime(job.ended || job.started)}.`);
  }
  return el("p", { class: "bad" },
    `Failed ${showTime(job.ended || job.started)} (${job.result}` +
    `${job.status ? `, exit status ${job.status}` : ""}): see its log.`);
}

/* A button that shows the last run's log of unit under it, with a button
 * to close it again. */
function logPanel(unit, label) {
  const panel = el("div", { class: "stack", hidden: true });
  const open = el("button", { class: "btn", type: "button", onclick: async () => {
    open.disabled = true;
    try {
      const r = await api("GET", `/api/server/log?unit=${encodeURIComponent(unit)}`);
      panel.replaceChildren(
        r.log ? el("pre", { class: "log" }, r.log)
              : el("p", { class: "empty" }, "No log: it has not run since the server started."),
        el("div", { class: "actions" },
          el("button", { class: "btn", type: "button", onclick: () => {
            panel.hidden = true;
            open.hidden = false;
          } }, "Close log")));
      panel.hidden = false;
      open.hidden = true;
    } catch (err) {
      handleError(err);
    } finally {
      open.disabled = false;
    }
  } }, label || "Log");
  return { button: open, panel };
}

/*
 * While job runs, checks back every few seconds (get() -> the job's new
 * state) and shows the page again when it ends. Stops when node is no
 * longer on the page.
 */
function watchJob(job, node, get, doneText) {
  if (!running(job)) return;
  const poll = async () => {
    if (!node.isConnected) return;
    try {
      const now = await get();
      if (!node.isConnected) return;
      if (running(now)) {
        setTimeout(poll, POLL_MS);
      } else {
        setStatus(doneText);
        refresh();
      }
    } catch (err) {
      handleError(err);
    }
  };
  setTimeout(poll, POLL_MS);
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

/* What predicts a disk's failure: ATA's bad sectors, NVMe's wear. Each
 * [text, colour] (colour "" if fine). */
function diskProblems(d) {
  const out = [];
  const count = (n, one, many) => [plural(n, one, many), n ? "bad" : ""];
  if (d.reallocated != null) out.push(count(d.reallocated, "reallocated sector", "reallocated sectors"));
  if (d.pending != null) out.push(count(d.pending, "pending sector", "pending sectors"));
  if (d.uncorrectable != null) {
    out.push(count(d.uncorrectable, "uncorrectable sector", "uncorrectable sectors"));
  }
  if (d.percent_used != null) out.push([`${d.percent_used} % worn`, fullness(d.percent_used / 100)]);
  if (d.spare != null) out.push([`${d.spare} % spare`, d.spare <= 10 ? "bad" : ""]);
  if (d.media_errors != null) out.push(count(d.media_errors, "media error", "media errors"));
  if (d.critical_warning) out.push([`critical warning ${d.critical_warning}`, "bad"]);
  return out;
}

function smartRow(d) {
  const health = d.passed === true ? el("span", { class: "ok" }, "passed")
    : d.passed === false ? el("strong", { class: "bad" }, "FAILING")
    : el("span", { class: "muted" }, "—");
  return el("tr", {},
    el("td", {}, d.device, el("div", { class: "muted small" }, d.model || "")),
    el("td", { class: "num" }, d.size ? showSize(d.size) : "—"),
    el("td", {}, health),
    el("td", { class: `num ${d.temperature != null ? tempClass(d.temperature) : ""}` },
       d.temperature != null ? `${d.temperature} °C` : "—"),
    el("td", { class: "num" }, d.hours != null ? showDuration(d.hours * 3600) : "—"),
    el("td", {}, d.error
      ? el("span", { class: "muted" }, d.error)
      : diskProblems(d).map(([text, cls]) => el("div", { class: cls || null }, text))));
}

/* SMART health of every disk; filled in when smartctl has answered (about
 * a second per disk), so the rest of the page does not wait for it. */
function smartCard() {
  const body = el("p", { class: "muted" }, "Reading the disks…");
  const card = el("section", { class: "card stack" }, el("h2", {}, "Disk health"), body);
  api("GET", "/api/server/smart").then((r) => {
    body.replaceWith(r.disks.length
      ? table([["Disk"], ["Size", "num"], ["SMART"], ["Temp.", "num"], ["Powered on", "num"],
               ["Signs of wear"]], r.disks.map(smartRow))
      : el("p", { class: "empty" }, "No disks found."));
  }).catch((err) => {
    if (!(err instanceof ApiError && err.status === 401 && handleError(err))) {
      body.replaceWith(el("p", { class: "bad" }, err.message));
    }
  });
  return card;
}

const DU_LABELS ={ "nylm data": "nylm's data", music: "Music", backups: "Backups",
                    docker: "Docker (images, containers, volumes)" };

function usageCard(du) {
  const job = du.job;
  const measure = passwordForm("/api/server/disk-usage", {},
    "Measures nylm's data, the music, the backups, Docker's folder and each backup " +
    "entry. On large folders this takes a while.",
    "Measure", "Measuring disk usage…");
  const log = logPanel(job.unit);
  const sizes = (du.sizes || []).slice().sort((a, b) => b.bytes - a.bytes);
  const card = el("section", { class: "card stack" },
    el("h2", {}, "Disk usage"),
    jobState(job),
    du.sizes == null || running(job) ? null
      : sizes.length
        ? table([["Folder"], ["Size", "num"]], sizes.map((s) => el("tr", {},
            el("td", {}, DU_LABELS[s.label] || `Backup entry ${s.label}`,
               el("div", { class: "path muted small" }, s.path)),
            el("td", { class: "num" }, showSize(s.bytes)))))
        : el("p", { class: "empty" }, "Nothing was measured: see its log."),
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "button", disabled: running(job) || du.busy,
                     onclick: () => measure.open() }, "Measure"),
      job.started ? log.button : null),
    du.busy && !running(job) ? el("p", { class: "muted" }, "Another job is running.") : null,
    measure,
    log.panel);
  watchJob(job, card, async () => (await api("GET", "/api/server/disk-usage")).job,
           "Disk usage measured");
  return card;
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
  const [h, du, audit] = await Promise.all([api("GET", "/api/server"),
                                             api("GET", "/api/server/disk-usage"),
                                             api("GET", "/api/server/audit")]);
  return shell("system",
    el("div", { class: "columns" }, hostCard(h), temperaturesCard(h.temperatures)),
    disksCard(h.mounts),
    smartCard(),
    usageCard(du),
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
