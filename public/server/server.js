"use strict";

/*
 * Server app (/server/). Sections by URL hash:
 *   #/system   the host (uptime, load, memory, temperatures, whether a
 *              reboot is needed), the services (systemd units), its disks,
 *              their SMART health, what takes the room on them, and the
 *              latest actions
 *   #/containers every Docker container: state, health, ports, use; restart
 *              it, read its log
 *   #/network  WireGuard: each peer, where it connects from, its last
 *              handshake and traffic, a name for it; the open ports
 *   #/updates  the packages that have an update (checkupdates), the system
 *              update (pacman -Syu) and its log, reboot
 *   #/backups  each backup entry (nylm's data first): its backups, Back up
 *              now, its log; how to copy them to another machine
 *
 * Reads never ask for anything; every button that changes the machine
 * asks for the password again, and the server records it (latest actions).
 * Long work is a job (a systemd unit): the page shows its state, checks
 * back while it runs, and shows its log.
 */

const TABS = [["system", "System"], ["containers", "Containers"], ["network", "Network"],
              ["updates", "Updates"], ["backups", "Backups"]];

/* What each recorded action was, by its name in the audit log. */
const ACTION_TEXT = {
  "disk-usage": "Measure disk usage",
  "docker-restart": "Restart container",
  "updates-check": "Check for updates",
  update: "System update",
  reboot: "Reboot",
  backup: "Backup",
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

/* A Log button and the panel it opens: load() resolves to the log's text
 * (emptyText if there is none); the panel has a button to close it. */
function logPanel(load, emptyText) {
  const panel = el("div", { class: "stack", hidden: true });
  const open = el("button", { class: "btn", type: "button", onclick: async () => {
    open.disabled = true;
    try {
      const text = await load();
      panel.replaceChildren(
        text ? el("pre", { class: "log" }, text) : el("p", { class: "empty" }, emptyText),
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
  } }, "Log");
  return { button: open, panel };
}

/* The log panel of a unit's last run. */
function unitLog(unit) {
  return logPanel(async () => (await api("GET", `/api/server/log?unit=${encodeURIComponent(unit)}`)).log,
                  "No log: it has not run since the server started.");
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
  const log = unitLog(job.unit);
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

/* A unit's state in a few words, and its colour. */
function unitState(u) {
  if (u.load === "not-found") return ["not installed", "bad"];
  if (u.active === "active") {
    return [u.sub === "exited" ? "done" : u.sub, "ok", u.since ? `since ${showTime(u.since)}` : ""];
  }
  if (u.active === "failed") return [`failed (${u.result})`, "bad", u.ended ? showTime(u.ended) : ""];
  if (running(u)) return [u.active, "warn", u.started ? `since ${showTime(u.started)}` : ""];
  /* inactive: a job that ran and ended (or has not run), or a service that
   * is stopped: that is wrong for a service someone watches. */
  const when = u.ended ? showTime(u.ended) : "";
  if (u.started && u.result !== "success") return [`stopped (${u.result})`, "bad", when];
  if (u.type === "oneshot") return [u.started ? "done" : "not run", "muted", when];
  return ["stopped", "bad", when];
}

function unitRow(u) {
  const [text, cls, when] = unitState(u);
  const log = unitLog(u.unit);
  return [
    el("tr", {},
      el("td", {}, u.unit, u.description && u.description !== u.unit
        ? el("div", { class: "muted small" }, u.description) : null),
      el("td", {}, el("strong", { class: cls }, text), when ? el("div", { class: "muted small" }, when) : null),
      el("td", {}, u.load === "not-found" ? null : log.button)),
    el("tr", { class: "log-row" }, el("td", { colspan: 3 }, log.panel)),
  ];
}

/* Services: NYLM_UNITS, then nylm's own units. */
function servicesCard(units) {
  return el("section", { class: "card stack" },
    el("h2", {}, "Services"),
    table([["Unit"], ["State"], [""]], units.flatMap(unitRow)),
    el("p", { class: "muted small" }, "More units: NYLM_UNITS in /etc/nylm.conf."));
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
  const [h, units, du, audit] = await Promise.all([api("GET", "/api/server"),
                                                    api("GET", "/api/server/units"),
                                                    api("GET", "/api/server/disk-usage"),
                                                    api("GET", "/api/server/audit")]);
  return shell("system",
    el("div", { class: "columns" }, hostCard(h), temperaturesCard(h.temperatures)),
    servicesCard(units.units),
    disksCard(h.mounts),
    smartCard(),
    usageCard(du),
    actionsSection(audit.actions));
}

/* ---- updates ------------------------------------------------------------- */

function packagesCard(u) {
  const check = passwordForm("/api/server/updates/check", {},
    "Looks for updates in a copy of the package databases (the system's own are not touched).",
    "Check", "Checking for updates…");
  const log = unitLog(u.check.unit);
  const list = u.packages;
  let content = null;
  if (!running(u.check) && list) {
    content = list.length
      ? table([["Package"], ["Installed"], ["New"]], list.map((p) => el("tr", {},
          el("td", {}, p.name), el("td", { class: "muted" }, p.old), el("td", {}, p.new))))
      : el("p", { class: "ok" }, "Everything is up to date.");
  }
  const card = el("section", { class: "card stack" },
    el("h2", {}, "Updates ", list && !running(u.check)
      ? el("span", { class: "count" }, plural(list.length, "package", "packages")) : null),
    jobState(u.check),
    content,
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "button", disabled: running(u.check),
                     onclick: () => check.open() }, "Check for updates"),
      u.check.started ? log.button : null),
    check,
    log.panel);
  watchJob(u.check, card, async () => (await api("GET", "/api/server/updates")).check,
           "Update check finished");
  return card;
}

function updateCard(u) {
  const start = passwordForm("/api/server/update", {},
    "Updates the keyring, then every package (pacman -Syu). It runs on its own: you can close " +
    "this page. Containers restart if Docker is updated.",
    "Update now", "Updating…");
  const log = unitLog(u.update.unit);
  const card = el("section", { class: "card stack" },
    el("h2", {}, "System update"),
    facts([["Last full upgrade", u.last_upgrade ? showTime(u.last_upgrade) : "not found"]]),
    jobState(u.update),
    el("p", { class: "muted" }, "Some updates need steps by hand: read the Arch news first."),
    el("div", { class: "actions" },
      el("a", { class: "btn", href: "https://archlinux.org/news/", target: "_blank",
                rel: "noopener noreferrer" }, "Arch news"),
      el("button", { class: "btn go", type: "button", disabled: running(u.update) || u.busy,
                     onclick: () => start.open() }, "Update now"),
      u.update.started ? log.button : null),
    u.busy && !running(u.update) ? el("p", { class: "muted" }, "Another job is running.") : null,
    start,
    log.panel);
  watchJob(u.update, card, async () => (await api("GET", "/api/server/updates")).update,
           "Update finished: see its log");
  return card;
}

/* After a reboot: checks every few seconds until the server is back up
 * (it answers, up for less than its uptime before), then shows the page. */
function waitForReboot(uptimeBefore) {
  const poll = async () => {
    try {
      const h = await api("GET", "/api/server");
      if (h.uptime < uptimeBefore) {
        setStatus("The server is back");
        refresh();
        return;
      }
    } catch (err) {
      if (err instanceof ApiError && err.status === 401) {
        handleError(err);
        return;
      }
      /* not up yet */
    }
    setTimeout(poll, 5000);
  };
  setTimeout(poll, 5000);
}

function rebootCard(u, uptime) {
  const reboot = passwordForm("/api/server/reboot", {},
    "Reboots the server now: every service and container stops and starts again. " +
    "This page comes back when the server is up.",
    "Reboot", "Rebooting… this page comes back when the server is up.",
    () => waitForReboot(uptime));
  return el("section", { class: "card stack" },
    el("h2", {}, "Reboot"),
    u.reboot_needed ? el("p", { class: "warn" }, "A newer kernel is installed: reboot to use it.")
                    : el("p", { class: "muted" }, "No reboot is needed."),
    el("div", { class: "actions" },
      el("button", { class: "btn danger", type: "button", disabled: u.busy,
                     onclick: () => reboot.open() }, "Reboot")),
    u.busy ? el("p", { class: "muted" }, "Not while a job is running.") : null,
    reboot);
}

async function updatesPage() {
  const [u, h] = await Promise.all([api("GET", "/api/server/updates"), api("GET", "/api/server")]);
  return shell("updates",
    packagesCard(u),
    el("div", { class: "columns" }, updateCard(u), rebootCard(u, h.uptime)));
}

/* ---- backups ------------------------------------------------------------- */

function entryItem(e, b) {
  const start = passwordForm("/api/server/backups/start", { name: e.name },
    e.name === "nylm"
      ? "Backs up nylm's data: its databases are copied safely while nylm runs."
      : `Backs up ${e.name}: the containers that use its folders stop while they are ` +
        "archived, and start again after.",
    "Back up", `Backing up ${e.name}…`);
  const log = unitLog(e.job.unit);
  const card = el("li", { class: "card stack" },
    el("header", { class: "line" },
      el("h3", {}, e.name === "nylm" ? "nylm's data" : e.name),
      el("span", { class: "count" }, plural(e.backups.length, "backup", "backups"))),
    el("div", {}, e.paths.map((p) => el("div", { class: "path muted small" }, p))),
    jobState(e.job),
    e.backups.length
      ? table([["Made"], ["Size", "num"]], e.backups.map((x) => el("tr", {},
          el("td", {}, showTime(x.time), el("div", { class: "path muted small" }, x.file)),
          el("td", { class: "num" }, showSize(x.size)))))
      : el("p", { class: "empty" }, "No backup yet."),
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "button",
                     disabled: running(e.job) || b.busy || !b.available,
                     onclick: () => start.open() }, "Back up now"),
      e.job.started ? log.button : null),
    start,
    log.panel);
  watchJob(e.job, card, async () => {
    const now = await api("GET", "/api/server/backups");
    return now.entries.find((x) => x.name === e.name).job;
  }, `Backup of ${e.name} finished`);
  return card;
}

/* How to copy the backups to another machine: it pulls them over SSH. */
function pullCard(b, host) {
  return el("section", { class: "card stack" },
    el("h2", {}, "Copy to your PC"),
    el("p", {}, "Your PC pulls the backups over SSH (the server never reaches your PC). " +
                "On the PC, after setting up its key as the README says:"),
    el("pre", { class: "log" }, `rsync -av YOU@${host}:${b.dir}/ ~/nylm-backups/`),
    el("p", { class: "muted small" },
      b.group ? `The backups can be read by the group ${b.group}.`
              : "Set NYLM_BACKUP_GROUP to a group your SSH user is in, so it can read them."));
}

async function backupsPage() {
  const [b, h] = await Promise.all([api("GET", "/api/server/backups"), api("GET", "/api/server")]);
  if (!b.dir) {
    return shell("backups", el("section", { class: "card stack" },
      el("h2", {}, "Backups"),
      el("p", { class: "warn" },
        "Set NYLM_BACKUP_DIR (and NYLM_BACKUP for the containers' folders) in " +
        "/etc/nylm.conf, then restart nylm.")));
  }
  return shell("backups",
    el("section", { class: "card stack" },
      el("h2", {}, "Backups"),
      el("p", {}, `Written to ${b.dir}; the newest ${b.keep} of each are kept. ` +
                  "Each is started by hand."),
      b.available ? null
        : el("p", { class: "bad" }, `${b.dir} is not there: is the drive mounted?`),
      b.busy ? el("p", { class: "muted" }, "A job is running: one at a time.") : null),
    el("ul", { class: "list cols" }, b.entries.map((e) => entryItem(e, b))),
    pullCard(b, h.hostname));
}

/* ---- containers ---------------------------------------------------------- */

const STATE_CLASS = { running: "ok", restarting: "warn", paused: "warn", created: "muted",
                      exited: "bad", dead: "bad", removing: "warn" };
const HEALTH_CLASS = { healthy: "ok", starting: "warn", unhealthy: "bad" };

/* "0.0.0.0:9001 → 80/tcp"; an exposed port without a host port alone. */
function portText(p) {
  return p.host ? `${p.host} → ${p.container}` : `${p.container} (not published)`;
}

function containerItem(c) {
  const restart = passwordForm("/api/server/containers/restart", { name: c.name },
    `Restarts ${c.name}: Docker stops it (at most 10 s) and starts it again.`,
    "Restart", `${c.name} restarted`);
  const log = logPanel(async () =>
    (await api("GET", `/api/server/containers/log?name=${encodeURIComponent(c.name)}`)).log,
    "The container wrote nothing.");
  const up = c.state === "running";
  return el("li", { class: "card stack" },
    el("header", { class: "line" },
      el("h3", {}, c.name),
      el("span", {},
        el("strong", { class: STATE_CLASS[c.state] || "" }, c.state),
        c.health ? [" · ", el("span", { class: HEALTH_CLASS[c.health] || "" }, c.health)] : null)),
    el("p", { class: "muted path" }, c.image || "?"),
    facts([
      up ? ["Up", c.started ? showDuration(Date.now() / 1000 - c.started) : "?"]
         : ["Stopped", `${c.finished ? showTime(c.finished) : "?"}, exit code ${c.exit_code}`],
      c.restarts ? ["Restarts", el("span", { class: "warn" }, String(c.restarts))] : null,
      c.ports.length ? ["Ports", c.ports.map((p) => el("div", {}, portText(p)))] : null,
      c.cpu != null ? ["CPU", `${c.cpu.toFixed(1)} %`] : null,
      c.memory != null ? ["Memory", `${showSize(c.memory)}` +
                                    (c.memory_percent != null ? ` (${c.memory_percent.toFixed(1)} %)` : "")]
                       : null,
    ]),
    el("div", { class: "actions" },
      el("button", { class: "btn", type: "button", onclick: () => restart.open() }, "Restart"),
      log.button),
    restart,
    log.panel);
}

async function containersPage() {
  const r = await api("GET", "/api/server/containers");
  const running = r.containers.filter((c) => c.state === "running").length;
  return shell("containers",
    el("section", { class: "section" },
      el("header", {},
        el("h2", {}, "Containers ", el("span", { class: "count" },
          `${running} of ${r.containers.length} running`))),
      r.containers.length
        ? el("ul", { class: "list cols" }, r.containers.map(containerItem))
        : el("p", { class: "empty" }, "No containers.")));
}

/* ---- network ------------------------------------------------------------- */

const KNOWN_PORTS = { 22: "SSH", 53: "DNS", 67: "DHCP", 68: "DHCP", 80: "HTTP", 123: "NTP",
                      443: "HTTPS", 631: "printing (CUPS)", 5353: "mDNS", 5355: "LLMNR" };

/* What listens on port/proto: nylm, WireGuard, a container, or a well
 * known service; "" if unknown. */
function portOwner(port, proto, ctx) {
  if (proto === "tcp" && port === ctx.nylmPort) return "nylm";
  const wg = ctx.wg && ctx.wg.interfaces.find((i) => i.port === port);
  if (proto === "udp" && wg) return `WireGuard (${wg.name})`;
  const names = (ctx.containers || [])
    .filter((c) => c.ports.some((p) => p.host && p.host.endsWith(`:${port}`) &&
                                       p.container.endsWith(`/${proto}`)))
    .map((c) => c.name);
  if (names.length) return `container ${names.join(", ")}`;
  return KNOWN_PORTS[port] || "";
}

/* One row per port and protocol, with every address it listens on. */
function portsCard(ports, ctx) {
  const rows = new Map();
  for (const p of ports) {
    const key = `${p.proto} ${p.port}`;
    if (!rows.has(key)) rows.set(key, { ...p, addresses: [] });
    if (!rows.get(key).addresses.includes(p.address)) rows.get(key).addresses.push(p.address);
  }
  const sorted = [...rows.values()].sort((a, b) => a.port - b.port || a.proto.localeCompare(b.proto));
  const local = (a) => a.startsWith("127.") || a === "::1";
  return el("section", { class: "card stack" },
    el("h2", {}, "Open ports"),
    el("p", { class: "muted small" },
      "Listening sockets on this machine. Only the router's forwarded ports reach the internet."),
    table([["Port", "num"], ["Proto"], ["On"], ["What"]], sorted.map((p) => el("tr", {},
      el("td", { class: "num" }, String(p.port)),
      el("td", {}, p.proto),
      el("td", { class: "muted small" }, p.addresses.every(local) ? "this machine only"
                                         : p.addresses.join(", ")),
      el("td", {}, portOwner(p.port, p.proto, ctx) || el("span", { class: "muted" }, "?"))))));
}

/* "2 min ago" */
function ago(ts) {
  const s = Math.max(0, Date.now() / 1000 - ts);
  return s < 60 ? "just now" : `${showDuration(s)} ago`;
}

/* A peer is connected while its handshakes are fresh: WireGuard makes a
 * new one every 2 minutes while there is traffic. */
const CONNECTED_SECONDS = 180;

function peerItem(p) {
  const name = el("input", { type: "text", maxlength: 100, value: p.name || "",
                             placeholder: "a name for this peer" });
  const connected = p.handshake && Date.now() / 1000 - p.handshake < CONNECTED_SECONDS;
  return el("li", { class: "card stack" },
    el("header", { class: "line" },
      el("h3", {}, p.name || "Unnamed peer"),
      connected ? el("strong", { class: "ok" }, "connected")
                : el("span", { class: "muted" }, p.handshake ? "idle" : "never connected")),
    facts([
      ["From", p.endpoint || el("span", { class: "muted" }, "not seen since WireGuard started")],
      ["Last handshake", p.handshake ? `${ago(p.handshake)} (${showTime(p.handshake)})` : "never"],
      ["VPN address", p.allowed_ips.join(", ") || "none"],
      ["Received", showSize(p.rx)],
      ["Sent", showSize(p.tx)],
      ["Key", el("span", { class: "path small muted" }, p.public_key)],
    ]),
    form({}, async () => {
      await api("POST", "/api/server/wireguard/name", { public_key: p.public_key,
                                                        name: name.value.trim() });
      setStatus(name.value.trim() ? "Peer named" : "Peer name removed");
      refresh();
    },
      el("div", { class: "row" }, name,
        el("button", { class: "btn", type: "submit" }, "Save name"))));
}

function wireguardSection(wg) {
  if (wg.error) {
    return el("section", { class: "section" }, el("h2", {}, "WireGuard"),
      el("p", { class: "bad" }, wg.error));
  }
  return el("section", { class: "section" },
    el("header", {}, el("h2", {}, "WireGuard ",
      el("span", { class: "count" }, plural(wg.peers.length, "peer", "peers")))),
    wg.interfaces.length
      ? el("p", { class: "muted" }, wg.interfaces.map((i) =>
          `${i.name}: listening on UDP ${i.port}, from the internet through the router.`).join(" "))
      : el("p", { class: "bad" }, "No WireGuard interface is up."),
    wg.peers.length ? el("ul", { class: "list cols" }, wg.peers.map(peerItem)) : null);
}

/* The result of a request, or {error} when it failed (but a lost login). */
async function orError(promise) {
  try {
    return await promise;
  } catch (err) {
    if (err instanceof ApiError && err.status === 401) throw err;
    return { error: err.message };
  }
}

async function networkPage() {
  const [ports, wg, docker] = await Promise.all([
    api("GET", "/api/server/ports"),
    orError(api("GET", "/api/server/wireguard")),
    orError(api("GET", "/api/server/containers")),
  ]);
  const ctx = { nylmPort: ports.nylm_port, wg: wg.error ? null : wg,
                containers: docker.error ? null : docker.containers };
  return shell("network",
    wireguardSection(wg),
    portsCard(ports.ports, ctx),
    docker.error ? el("p", { class: "muted small" }, `Containers unknown: ${docker.error}`) : null);
}

/* ---- routing ------------------------------------------------------------- */

function route(parts) {
  const [section, rest] = parts;
  if (section === "system" && rest === undefined) return systemPage();
  if (section === "containers" && rest === undefined) return containersPage();
  if (section === "network" && rest === undefined) return networkPage();
  if (section === "updates" && rest === undefined) return updatesPage();
  if (section === "backups" && rest === undefined) return backupsPage();
  go("#/system");
  return null;
}

startApp(route);
