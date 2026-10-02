"use strict";

/*
 * nylm frontend. The server only serves files and JSON; everything the user
 * sees is built here. Rule: data goes into the page with textContent (via
 * el()), never innerHTML.
 *
 * Pages are addressed by the URL hash:
 *   #/                      home: one tile per app
 *   #/plants/due            what to do, soonest first
 *   #/plants/plants[/ID]    plants, or one plant with its care rules
 *   #/plants/journal[/ID]   a plant's care log and notes
 *   #/plants/types          care types
 */

const app = document.getElementById("app");
const statusLine = document.getElementById("status");
const logoutButton = document.getElementById("logout");

/* ---- helpers ---------------------------------------------------------- */

/* el("button", {class: "btn", onclick: f}, "text", childNode, ...) */
function el(tag, attrs, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs || {})) {
    if (key.startsWith("on")) node.addEventListener(key.slice(2), value);
    else if (value === true) node.setAttribute(key, "");
    else if (value !== false && value != null) node.setAttribute(key, value);
  }
  for (const child of children.flat()) {
    if (child == null || child === false) continue;
    node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return node;
}

function setStatus(text, isError) {
  statusLine.textContent = text || "";
  statusLine.classList.toggle("error", Boolean(isError));
}

class ApiError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

/* Calls the JSON API. Resolves to the parsed body (null for 204). */
async function api(method, path, body) {
  const options = { method, headers: {}, credentials: "same-origin" };
  if (body !== undefined || method !== "GET") {
    options.headers["Content-Type"] = "application/json";
    if (body !== undefined) options.body = JSON.stringify(body);
  }
  const res = await fetch(path, options);
  const data = res.status === 204 ? null : await res.json().catch(() => null);
  if (!res.ok) {
    throw new ApiError(res.status, (data && data.error) || res.statusText);
  }
  return data;
}

/* A labelled form control. */
function field(label, control, hint) {
  return el("label", { class: "field" }, el("span", {}, label), control,
            hint ? el("span", { class: "hint" }, hint) : null);
}

/*
 * A form whose submit runs action(form). While it runs the form's buttons
 * are disabled; an error is shown in the form's .form-error line.
 */
function form(attrs, action, ...children) {
  const error = el("p", { class: "form-error", role: "alert" });
  const node = el("form", {
    ...attrs,
    class: "form " + (attrs.class || ""),
    onsubmit: async (e) => {
      e.preventDefault();
      const buttons = node.querySelectorAll("button");
      buttons.forEach((b) => { b.disabled = true; });
      error.textContent = "";
      try {
        await action(node);
      } catch (err) {
        /* A lost session goes to the login form; anything else shows here. */
        if (!(err instanceof ApiError && err.status === 401 && handleError(err))) {
          error.textContent = err.message;
        }
      } finally {
        buttons.forEach((b) => { b.disabled = false; });
      }
    },
  }, ...children, error);
  return node;
}

/* A form error raised before anything is sent. */
function invalid(message) {
  return new Error(message);
}

/* Asks before something that cannot be undone. */
function sure(question) {
  return window.confirm(question);
}

/* ---- dates and status -------------------------------------------------- */

const MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
const DAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];

function pad2(n) {
  return String(n).padStart(2, "0");
}

/* Today as "YYYY-MM-DD" in local time. */
function today() {
  const d = new Date();
  return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
}

/* "2026-10-04" -> "Sun 4 Oct" (this year) or "4 Oct 2027". */
function showDate(iso) {
  const [y, m, d] = iso.split("-").map(Number);
  const date = new Date(y, m - 1, d);
  if (y === new Date().getFullYear()) return `${DAYS[date.getDay()]} ${d} ${MONTHS[m - 1]}`;
  return `${d} ${MONTHS[m - 1]} ${y}`;
}

/* "3 Mar" for a month/day pair. */
function showMonthDay(month, day) {
  return `${day} ${MONTHS[month - 1]}`;
}

/* How soon something is due, in words. */
function dueText(daysLeft, due) {
  if (daysLeft == null) return "Paused";
  if (daysLeft < -1) return `${-daysLeft} days late`;
  if (daysLeft === -1) return "1 day late";
  if (daysLeft === 0) return "Today";
  if (daysLeft === 1) return "Tomorrow";
  if (daysLeft <= 7) return `In ${daysLeft} days`;
  return showDate(due);
}

/* The colour class for how soon something is due. */
function dueTone(daysLeft) {
  if (daysLeft == null) return "tone-paused";
  if (daysLeft < 0) return "tone-late";
  if (daysLeft === 0) return "tone-today";
  if (daysLeft <= 7) return "tone-soon";
  return "tone-later";
}

function duePill(daysLeft, due) {
  return el("span", { class: "pill " + dueTone(daysLeft) }, dueText(daysLeft, due));
}

function plural(n, one, many) {
  return `${n} ${n === 1 ? one : many}`;
}

/* ---- routing ----------------------------------------------------------- */

/* Each render bumps this; a render that finds it changed was overtaken. */
let renderSeq = 0;

function go(hash) {
  if (location.hash === hash) route();
  else location.hash = hash;
}

async function route() {
  const seq = ++renderSeq;
  const parts = location.hash.replace(/^#\/?/, "").split("/").filter(Boolean);
  const id = parts[2] !== undefined ? Number(parts[2]) : null;
  if (id !== null && !(Number.isInteger(id) && id > 0)) {
    go("#/");
    return;
  }
  logoutButton.hidden = false;
  try {
    let page;
    if (parts.length === 0) page = await homePage();
    else if (parts[0] === "plants" && parts[1] === "due") page = await duePage();
    else if (parts[0] === "plants" && parts[1] === "plants" && id) page = await plantPage(id);
    else if (parts[0] === "plants" && parts[1] === "plants") page = await plantsPage();
    else if (parts[0] === "plants" && parts[1] === "journal") page = await journalPage(id);
    else if (parts[0] === "plants" && parts[1] === "types") page = await typesPage();
    else {
      go("#/");
      return;
    }
    if (seq === renderSeq) app.replaceChildren(page);
  } catch (err) {
    if (seq === renderSeq) handleError(err);
  }
}

window.addEventListener("hashchange", route);

/* ---- home -------------------------------------------------------------- */

/* Pills summing up a due list: late, today, this week. */
function dueSummary(due) {
  const late = due.filter((d) => d.days_left < 0).length;
  const now = due.filter((d) => d.days_left === 0).length;
  const week = due.filter((d) => d.days_left > 0 && d.days_left <= 7).length;
  if (late + now + week === 0) {
    return [el("span", { class: "pill tone-done" }, "Nothing due this week")];
  }
  return [
    late ? el("span", { class: "pill tone-late" }, `${late} late`) : null,
    now ? el("span", { class: "pill tone-today" }, `${now} today`) : null,
    week ? el("span", { class: "pill tone-soon" }, `${week} this week`) : null,
  ];
}

async function homePage() {
  const due = await api("GET", "/api/plants/due");
  return el("section", {},
    el("h1", {}, "Apps"),
    el("nav", { class: "tiles", "aria-label": "Apps" },
      el("a", { class: "tile", href: "#/plants/due" },
        el("h2", {}, "Plants"),
        el("div", { class: "pills" }, dueSummary(due)))));
}

/* ---- plants: shell ----------------------------------------------------- */

const PLANT_TABS = [
  ["due", "Due"],
  ["plants", "Plants"],
  ["journal", "Journal"],
  ["types", "Care types"],
];

function plantsShell(active, ...content) {
  return el("section", {},
    el("header", { class: "app-head" },
      el("h1", {}, "Plants"),
      el("nav", { class: "tabs", "aria-label": "Plants sections" },
        PLANT_TABS.map(([key, label]) =>
          el("a", { href: `#/plants/${key}`, "aria-current": key === active ? "page" : null },
             label)))),
    ...content);
}

/* Logs care done on a date, then shows the page again. */
async function logCare(plantId, plantName, typeId, typeName, date) {
  await api("POST", "/api/plants/log/add",
            { plant_id: plantId, care_type_id: typeId, date, note: "" });
  setStatus(`Logged ${typeName.toLowerCase()} for ${plantName}`);
  route();
}

/*
 * The two ways to mark care as done: today, or on an earlier date (which
 * opens a small date form).
 */
function doneButtons(plantId, plantName, typeId, typeName) {
  const date = el("input", { type: "date", required: true, max: today(), value: today() });
  const later = form({ hidden: true }, async () => {
    await logCare(plantId, plantName, typeId, typeName, date.value);
  },
    el("div", { class: "row" },
      field("Done on", date),
      el("button", { class: "btn go", type: "submit" }, "Log"),
      el("button", { class: "btn quiet", type: "button",
                     onclick: () => { later.hidden = true; } }, "Cancel")));
  const nowButton = el("button", {
    class: "btn go", type: "button",
    onclick: async () => {
      nowButton.disabled = true;
      try {
        await logCare(plantId, plantName, typeId, typeName, today());
      } catch (err) {
        handleError(err);
        nowButton.disabled = false;
      }
    },
  }, "Done today");
  return [
    el("div", { class: "actions" },
      nowButton,
      el("button", { class: "btn", type: "button",
                     onclick: () => { later.hidden = false; date.focus(); } },
         "Done on a date…")),
    later,
  ];
}

/* ---- plants: due ------------------------------------------------------- */

const DUE_GROUPS = [
  ["Late", (d) => d < 0],
  ["Today", (d) => d === 0],
  ["This week", (d) => d > 0 && d <= 7],
  ["Later", (d) => d > 7],
];

function dueRow(item) {
  return el("li", { class: "card marked due-row " + dueTone(item.days_left) },
    el("div", { class: "what" },
      el("a", { href: `#/plants/plants/${item.plant_id}` }, item.plant),
      el("span", { class: "muted" }, item.care_type)),
    el("span", { class: "when" }, duePill(item.days_left, item.due)),
    el("div", { class: "done" },
      doneButtons(item.plant_id, item.plant, item.care_type_id, item.care_type)));
}

async function duePage() {
  const due = await api("GET", "/api/plants/due");
  if (due.length === 0) {
    return plantsShell("due",
      el("p", { class: "empty" }, "Nothing is scheduled. Open a plant in ",
         el("a", { href: "#/plants/plants" }, "Plants"), " and add a care rule."));
  }
  return plantsShell("due",
    DUE_GROUPS.map(([title, test]) => {
      const items = due.filter((d) => test(d.days_left));
      if (items.length === 0) return null;
      return el("section", { class: "section" },
        el("header", {}, el("h2", {}, title, " ", el("span", { class: "count" }, items.length))),
        el("ul", { class: "list" }, items.map(dueRow)));
    }));
}

/* ---- plants: list ------------------------------------------------------ */

const PLANT_LIMITS = { name: 100, species: 100, location: 100, notes: 4000 };

/* Inputs for a plant's details, filled from plant (or empty). */
function plantInputs(plant) {
  const p = plant || {};
  return {
    name: el("input", { name: "name", required: true, maxlength: PLANT_LIMITS.name,
                        value: p.name || "" }),
    species: el("input", { name: "species", maxlength: PLANT_LIMITS.species,
                           value: p.species || "" }),
    location: el("input", { name: "location", maxlength: PLANT_LIMITS.location,
                            value: p.location || "" }),
    acquired: el("input", { name: "acquired", type: "date", max: today(),
                            value: p.acquired || "" }),
    notes: el("textarea", { name: "notes", maxlength: PLANT_LIMITS.notes }, p.notes || ""),
  };
}

function plantFields(inputs) {
  return [
    field("Name", inputs.name),
    el("div", { class: "row" }, field("Species", inputs.species),
       field("Location", inputs.location)),
    field("Acquired", inputs.acquired, "Leave empty if you don't know."),
    field("Notes", inputs.notes),
  ];
}

function plantBody(inputs) {
  return {
    name: inputs.name.value.trim(),
    species: inputs.species.value.trim(),
    location: inputs.location.value.trim(),
    acquired: inputs.acquired.value || null,
    notes: inputs.notes.value,
  };
}

/* The most urgent due item of each plant, by plant id. */
function mostUrgent(due) {
  const first = new Map();
  for (const item of due) {
    const seen = first.get(item.plant_id);
    if (!seen || item.days_left < seen.days_left) first.set(item.plant_id, item);
  }
  return first;
}

function plantCard(plant, urgent) {
  const where = [plant.species, plant.location].filter(Boolean).join(", ");
  return el("li", {},
    el("a", { class: "card marked plant-card " + (urgent ? dueTone(urgent.days_left) : ""),
              href: `#/plants/plants/${plant.id}` },
      el("h3", {}, plant.name),
      urgent
        ? el("span", {}, duePill(urgent.days_left, urgent.due))
        : el("span", { class: "pill tone-paused" }, "Nothing scheduled"),
      where ? el("span", { class: "muted" }, where) : null,
      urgent ? el("span", { class: "muted" }, urgent.care_type) : null));
}

function addPlantForm() {
  const inputs = plantInputs(null);
  const node = form({ class: "card", hidden: true }, async () => {
    const created = await api("POST", "/api/plants/add", plantBody(inputs));
    setStatus(`Added ${inputs.name.value.trim()}`);
    go(`#/plants/plants/${created.id}`);
  },
    el("h2", {}, "New plant"),
    plantFields(inputs),
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "submit" }, "Add plant"),
      el("button", { class: "btn quiet", type: "button",
                     onclick: () => { node.hidden = true; } }, "Cancel")));
  return node;
}

async function setPlantArchived(plant, archived) {
  await api("POST", "/api/plants/archive", { id: plant.id, archived });
  setStatus(`${archived ? "Archived" : "Restored"} ${plant.name}`);
  route();
}

async function plantsPage() {
  const [data, due] = await Promise.all([api("GET", "/api/plants"),
                                         api("GET", "/api/plants/due")]);
  const urgent = mostUrgent(due);
  const active = data.plants.filter((p) => !p.archived);
  const archived = data.plants.filter((p) => p.archived);
  const addForm = addPlantForm();

  return plantsShell("plants",
    el("section", { class: "section" },
      el("header", {},
        el("h2", {}, "Plants ", el("span", { class: "count" }, active.length)),
        el("button", { class: "btn go", type: "button",
                       onclick: () => { addForm.hidden = false; addForm.querySelector("input").focus(); } },
           "Add plant")),
      addForm,
      active.length
        ? el("ul", { class: "list" }, active.map((p) => plantCard(p, urgent.get(p.id))))
        : el("p", { class: "empty" }, "No plants yet. Add your first one.")),
    archived.length
      ? el("section", { class: "section" },
          el("header", {}, el("h2", {}, "Archived ", el("span", { class: "count" }, archived.length))),
          el("ul", { class: "list" }, archived.map((p) =>
            el("li", { class: "well due-row" },
              el("a", { href: `#/plants/plants/${p.id}` }, p.name),
              el("button", { class: "btn", type: "button",
                             onclick: () => setPlantArchived(p, false).catch(handleError) },
                 "Restore")))))
      : null);
}

/* ---- plants: one plant and its rules ----------------------------------- */

/* "Every 4 days", "Once a year on 1 Mar", "Only in seasons". */
function scheduleText(rule) {
  if (rule.yearly_month) return `Once a year on ${showMonthDay(rule.yearly_month, rule.yearly_day)}`;
  if (rule.interval_days) return `Every ${plural(rule.interval_days, "day", "days")}`;
  return "Only in the seasons below";
}

function seasonText(p) {
  const span = `${showMonthDay(p.start_month, p.start_day)} – ${showMonthDay(p.end_month, p.end_day)}`;
  return `${span}: ${p.interval_days ? "every " + plural(p.interval_days, "day", "days") : "paused"}`;
}

function ruleCard(plant, rule, types) {
  const type = types.find((t) => t.id === rule.care_type_id);
  const typeArchived = Boolean(type && type.archived);
  const editor = el("div", { hidden: true });
  const view = el("div", { class: "rule" },
    el("header", {},
      el("h3", {}, rule.care_type),
      duePill(rule.days_left, rule.due)),
    el("p", {}, scheduleText(rule)),
    rule.periods.length
      ? el("ul", { class: "seasons" }, rule.periods.map((p) => el("li", {}, seasonText(p))))
      : null,
    el("p", { class: "muted" },
       rule.last_done ? `Last done ${showDate(rule.last_done)}` : "Not done yet"),
    typeArchived
      ? el("p", { class: "hint" }, `${rule.care_type} is archived: restore it in `,
           el("a", { href: "#/plants/types" }, "Care types"), " to log it.")
      : rule.due ? doneButtons(plant.id, plant.name, rule.care_type_id, rule.care_type) : null,
    el("div", { class: "actions" },
      el("button", { class: "btn", type: "button", onclick: () => {
        editor.replaceChildren(ruleForm(plant, rule, types, () => {
          editor.hidden = true;
          view.hidden = false;
        }));
        view.hidden = true;
        editor.hidden = false;
      } }, "Edit rule"),
      el("button", { class: "btn danger", type: "button", onclick: async () => {
        if (!sure(`Delete the ${rule.care_type.toLowerCase()} rule for ${plant.name}? ` +
                  "Its log entries stay in the journal.")) return;
        try {
          await api("POST", "/api/plants/rules/delete",
                    { plant_id: plant.id, care_type_id: rule.care_type_id });
          setStatus(`Deleted the ${rule.care_type.toLowerCase()} rule`);
          route();
        } catch (err) {
          handleError(err);
        }
      } }, "Delete rule")));
  return el("li", { class: "card marked " + dueTone(rule.days_left) }, view, editor);
}

function monthSelect(value) {
  return el("select", { "aria-label": "Month" },
    MONTHS.map((name, i) => el("option", { value: i + 1, selected: i + 1 === value }, name)));
}

function dayInput(value) {
  return el("input", { type: "number", min: 1, max: 31, required: true, "aria-label": "Day",
                       value: value || 1 });
}

/* One editable season: a month/day range and its interval or "paused". */
function seasonRow(season, onRemove) {
  const s = season || { start_month: 11, start_day: 1, end_month: 2, end_day: 28,
                        interval_days: 14 };
  const startMonth = monthSelect(s.start_month), startDay = dayInput(s.start_day);
  const endMonth = monthSelect(s.end_month), endDay = dayInput(s.end_day);
  const kind = el("select", { "aria-label": "In this season" },
    el("option", { value: "every", selected: Boolean(s.interval_days) }, "every"),
    el("option", { value: "paused", selected: !s.interval_days }, "paused"));
  const days = el("input", { type: "number", min: 1, max: 3650, "aria-label": "Days",
                             value: s.interval_days || 7 });
  const daysLabel = el("span", {}, "days");
  const sync = () => { days.hidden = daysLabel.hidden = kind.value === "paused"; };
  kind.addEventListener("change", sync);
  sync();
  const row = el("div", { class: "season-row well" },
    el("span", {}, "From"), startDay, startMonth,
    el("span", {}, "to"), endDay, endMonth,
    kind, days, daysLabel,
    el("button", { class: "btn quiet", type: "button", onclick: () => onRemove(row) },
       "Remove season"));
  row.read = () => ({
    start_month: Number(startMonth.value), start_day: Number(startDay.value),
    end_month: Number(endMonth.value), end_day: Number(endDay.value),
    interval_days: kind.value === "paused" ? null : Number(days.value),
  });
  return row;
}

/*
 * The form to add a rule (rule null) or change one. The schedule is one of:
 * every N days, only in seasons (paused otherwise), or once a year.
 */
function ruleForm(plant, rule, types, onCancel) {
  const r = rule || { interval_days: 7, yearly_month: null, yearly_day: null, periods: [] };
  const typeSelect = rule ? null : el("select", { required: true, "aria-label": "Care type" },
    types.map((t) => el("option", { value: t.id }, t.name)));

  const initial = r.yearly_month ? "yearly" : r.interval_days ? "every" : "seasons";
  const mode = (value, label) => el("label", { class: "choice" },
    el("input", { type: "radio", name: "mode", value, checked: value === initial }), label);
  const interval = el("input", { type: "number", min: 1, max: 3650,
                                 value: r.interval_days || 7, "aria-label": "Days" });
  const yearMonth = monthSelect(r.yearly_month || 3);
  const yearDay = dayInput(r.yearly_day || 1);

  const seasonList = el("div", { class: "stack" });
  const removeSeason = (row) => row.remove();
  for (const p of r.periods) seasonList.append(seasonRow(p, removeSeason));
  const seasons = el("div", { class: "stack" },
    el("p", { class: "hint" },
       "Seasons change the interval or pause it between two dates, every year."),
    seasonList,
    el("div", { class: "actions" },
      el("button", { class: "btn", type: "button",
                     onclick: () => seasonList.append(seasonRow(null, removeSeason)) },
         "Add season")));

  const everyRow = el("div", { class: "row" }, el("span", {}, "Every"), interval,
                      el("span", {}, "days, outside the seasons below"));
  const yearlyRow = el("div", { class: "row" }, el("span", {}, "On"), yearDay, yearMonth);
  const seasonsOnly = el("p", { class: "hint" }, "Not due outside the seasons below.");

  const node = form({ class: "stack" }, async (f) => {
    const chosen = f.querySelector("input[name=mode]:checked").value;
    if (chosen === "every" && !(Number(interval.value) >= 1)) {
      throw invalid("Enter how many days between each time.");
    }
    const periods = chosen === "yearly" ? [] : [...seasonList.children].map((row) => row.read());
    if (chosen === "seasons" && !periods.some((p) => p.interval_days)) {
      throw invalid("Add a season that is not paused, or choose another schedule.");
    }
    const typeId = rule ? rule.care_type_id : Number(typeSelect.value);
    await api("POST", "/api/plants/rules/save", {
      plant_id: plant.id,
      care_type_id: typeId,
      interval_days: chosen === "every" ? Number(interval.value) : null,
      yearly_month: chosen === "yearly" ? Number(yearMonth.value) : null,
      yearly_day: chosen === "yearly" ? Number(yearDay.value) : null,
      periods,
    });
    setStatus("Saved the rule");
    route();
  },
    el("h3", {}, rule ? `${rule.care_type} rule` : "New care rule"),
    typeSelect ? field("Care type", typeSelect) : null,
    el("fieldset", { class: "stack well" },
      el("legend", { class: "hint" }, "Schedule"),
      mode("every", "Every few days"),
      mode("seasons", "Only in some seasons"),
      mode("yearly", "Once a year")),
    everyRow, seasonsOnly, yearlyRow, seasons,
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "submit" }, "Save rule"),
      el("button", { class: "btn quiet", type: "button", onclick: onCancel }, "Cancel")));

  /* Show only the inputs of the chosen schedule. */
  const sync = () => {
    const chosen = node.querySelector("input[name=mode]:checked").value;
    everyRow.hidden = chosen !== "every";
    seasonsOnly.hidden = chosen !== "seasons";
    yearlyRow.hidden = chosen !== "yearly";
    seasons.hidden = chosen === "yearly";
  };
  node.addEventListener("change", (e) => { if (e.target.name === "mode") sync(); });
  sync();
  return node;
}

async function plantPage(id) {
  const [plant, data] = await Promise.all([api("GET", `/api/plants/plant?id=${id}`),
                                           api("GET", "/api/plants")]);
  const ruled = new Set(plant.rules.map((r) => r.care_type_id));
  const freeTypes = data.care_types.filter((t) => !t.archived && !ruled.has(t.id));

  const addRule = el("div", { hidden: true });
  const inputs = plantInputs(plant);
  const details = form({ class: "card" }, async () => {
    await api("POST", "/api/plants/update", { id: plant.id, ...plantBody(inputs) });
    setStatus(`Saved ${inputs.name.value.trim()}`);
    route();
  },
    plantFields(inputs),
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "submit" }, "Save details")));

  return plantsShell("plants",
    el("p", {}, el("a", { href: "#/plants/plants" }, "All plants")),
    el("header", { class: "section" },
      el("h2", {}, plant.name),
      plant.archived ? el("p", { class: "muted" }, "Archived: not shown in Due.") : null),

    el("section", { class: "section" },
      el("header", {},
        el("h2", {}, "Care rules ", el("span", { class: "count" }, plant.rules.length)),
        freeTypes.length
          ? el("button", { class: "btn go", type: "button", onclick: () => {
              addRule.replaceChildren(el("div", { class: "card" },
                ruleForm(plant, null, freeTypes, () => { addRule.hidden = true; })));
              addRule.hidden = false;
            } }, "Add care rule")
          : null),
      freeTypes.length === 0
        ? el("p", { class: "hint" }, "Every care type has a rule here. Add more types in ",
             el("a", { href: "#/plants/types" }, "Care types"), ".")
        : null,
      addRule,
      plant.rules.length
        ? el("ul", { class: "list" }, plant.rules.map((r) => ruleCard(plant, r, data.care_types)))
        : el("p", { class: "empty" }, "No care rules yet.")),

    el("section", { class: "section" },
      el("header", {},
        el("h2", {}, "Journal"),
        el("a", { href: `#/plants/journal/${plant.id}` }, "Open journal"))),

    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Details")),
      details,
      el("div", { class: "actions" },
        plant.archived
          ? el("button", { class: "btn", type: "button",
                           onclick: () => setPlantArchived(plant, false).catch(handleError) },
               "Restore plant")
          : el("button", { class: "btn danger", type: "button",
                           onclick: () => setPlantArchived(plant, true).catch(handleError) },
               "Archive plant"))));
}

/* ---- plants: journal --------------------------------------------------- */

/* A care type select; "" means a plain note. */
function typeSelect(types, selected) {
  return el("select", { "aria-label": "Care type" },
    el("option", { value: "", selected: selected == null }, "Note only"),
    types.map((t) => el("option", { value: t.id, selected: t.id === selected }, t.name)));
}

/* Inputs for a log entry, filled from entry (or today / note only). */
function entryInputs(entry, types) {
  const e = entry || { date: today(), care_type_id: null, note: "" };
  return {
    date: el("input", { type: "date", required: true, max: today(), value: e.date }),
    type: typeSelect(types, e.care_type_id),
    note: el("textarea", { maxlength: 4000 }, e.note),
  };
}

function entryFields(inputs) {
  return [
    el("div", { class: "row" }, field("Date", inputs.date), field("What", inputs.type)),
    field("Note", inputs.note),
  ];
}

function entryBody(inputs) {
  return {
    date: inputs.date.value,
    care_type_id: inputs.type.value === "" ? null : Number(inputs.type.value),
    note: inputs.note.value,
  };
}

function entryCard(entry, typeNames, types) {
  const name = entry.care_type_id == null ? null : typeNames.get(entry.care_type_id);
  /* An entry may keep an archived type, so offer it while editing. */
  const editTypes = types.filter((t) => !t.archived || t.id === entry.care_type_id);
  const editor = el("div", { hidden: true });
  const view = el("div", { class: "entry" },
    el("header", {},
      el("strong", {}, showDate(entry.date)),
      name ? el("span", { class: "pill tone-done" }, name)
           : el("span", { class: "pill tone-paused" }, "Note")),
    entry.note ? el("p", { class: "note" }, entry.note) : null,
    el("div", { class: "actions" },
      el("button", { class: "btn", type: "button", onclick: () => {
        const inputs = entryInputs(entry, editTypes);
        editor.replaceChildren(form({}, async () => {
          await api("POST", "/api/plants/log/update", { id: entry.id, ...entryBody(inputs) });
          setStatus("Saved the entry");
          route();
        },
          entryFields(inputs),
          el("div", { class: "actions" },
            el("button", { class: "btn go", type: "submit" }, "Save entry"),
            el("button", { class: "btn quiet", type: "button", onclick: () => {
              editor.hidden = true;
              view.hidden = false;
            } }, "Cancel"))));
        view.hidden = true;
        editor.hidden = false;
      } }, "Edit"),
      el("button", { class: "btn danger", type: "button", onclick: async () => {
        if (!sure(`Delete the entry of ${showDate(entry.date)}?`)) return;
        try {
          await api("POST", "/api/plants/log/delete", { id: entry.id });
          setStatus("Deleted the entry");
          route();
        } catch (err) {
          handleError(err);
        }
      } }, "Delete")));
  return el("li", { class: "card" }, view, editor);
}

async function journalPage(id) {
  const data = await api("GET", "/api/plants");
  const plants = data.plants;
  if (plants.length === 0) {
    return plantsShell("journal",
      el("p", { class: "empty" }, "Add a plant in ",
         el("a", { href: "#/plants/plants" }, "Plants"), " to start its journal."));
  }
  const plant = plants.find((p) => p.id === id) ||
                plants.find((p) => !p.archived) || plants[0];
  if (plant.id !== id) {
    history.replaceState(null, "", `#/plants/journal/${plant.id}`);
  }

  const log = await api("GET", `/api/plants/log?plant_id=${plant.id}`);
  const types = data.care_types;
  const typeNames = new Map(types.map((t) => [t.id, t.name]));
  const active = types.filter((t) => !t.archived);

  const picker = el("select", {
    "aria-label": "Plant",
    onchange: (e) => go(`#/plants/journal/${e.target.value}`),
  }, plants.map((p) =>
    el("option", { value: p.id, selected: p.id === plant.id },
       p.archived ? `${p.name} (archived)` : p.name)));

  const inputs = entryInputs(null, active);
  const add = form({ class: "card" }, async () => {
    await api("POST", "/api/plants/log/add", { plant_id: plant.id, ...entryBody(inputs) });
    setStatus("Added the entry");
    route();
  },
    el("h2", {}, "New entry"),
    entryFields(inputs),
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "submit" }, "Add entry")));

  const list = el("ul", { class: "list" }, log.entries.map((e) => entryCard(e, typeNames, types)));
  let more = log.more;
  const olderButton = el("button", { class: "btn", type: "button", onclick: async () => {
    olderButton.disabled = true;
    try {
      const last = list.lastElementChild.entryId;
      const older = await api("GET", `/api/plants/log?plant_id=${plant.id}&before=${last}`);
      for (const e of older.entries) list.append(withId(entryCard(e, typeNames, types), e.id));
      more = older.more;
      olderButton.hidden = !more;
    } catch (err) {
      handleError(err);
    } finally {
      olderButton.disabled = false;
    }
  } }, "Show older entries");
  [...list.children].forEach((li, i) => withId(li, log.entries[i].id));
  olderButton.hidden = !more;

  return plantsShell("journal",
    el("div", { class: "stack" },
      field("Plant", picker),
      el("p", {}, el("a", { href: `#/plants/plants/${plant.id}` }, `${plant.name}: care rules`)),
      add,
      el("section", { class: "section" },
        el("header", {}, el("h2", {}, "Entries")),
        log.entries.length ? list : el("p", { class: "empty" }, "Nothing written yet."),
        el("div", { class: "actions" }, olderButton))));
}

/* Remembers which log entry a list item shows, for paging. */
function withId(li, id) {
  li.entryId = id;
  return li;
}

/* ---- plants: care types ------------------------------------------------ */

async function setTypeArchived(type, archived) {
  await api("POST", "/api/plants/types/archive", { id: type.id, archived });
  setStatus(`${archived ? "Archived" : "Restored"} ${type.name}`);
  route();
}

function typeRow(type) {
  const name = el("input", { value: type.name, required: true, maxlength: 50,
                             "aria-label": "Name" });
  return el("li", {},
    form({ class: "card" }, async () => {
      await api("POST", "/api/plants/types/update", { id: type.id, name: name.value.trim() });
      setStatus(`Renamed to ${name.value.trim()}`);
      route();
    },
      el("div", { class: "row" },
        name,
        el("button", { class: "btn", type: "submit" }, "Rename"),
        el("button", { class: "btn danger", type: "button",
                       onclick: () => setTypeArchived(type, true).catch(handleError) },
           "Archive"))));
}

async function typesPage() {
  const data = await api("GET", "/api/plants");
  const active = data.care_types.filter((t) => !t.archived);
  const archived = data.care_types.filter((t) => t.archived);
  const name = el("input", { required: true, maxlength: 50, "aria-label": "New care type",
                             placeholder: "e.g. Watering" });

  return plantsShell("types",
    el("section", { class: "section" },
      el("p", { class: "hint" },
         "Care types are what you do to plants. Each plant gets its own rule per type."),
      form({ class: "card" }, async () => {
        await api("POST", "/api/plants/types/add", { name: name.value.trim() });
        setStatus(`Added ${name.value.trim()}`);
        route();
      },
        el("div", { class: "row" },
          name,
          el("button", { class: "btn go", type: "submit" }, "Add care type")))),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Care types ", el("span", { class: "count" }, active.length))),
      active.length
        ? el("ul", { class: "list" }, active.map(typeRow))
        : el("p", { class: "empty" }, "No care types yet.")),
    archived.length
      ? el("section", { class: "section" },
          el("header", {}, el("h2", {}, "Archived ", el("span", { class: "count" }, archived.length))),
          el("ul", { class: "list" }, archived.map((t) =>
            el("li", { class: "well due-row" },
              el("span", {}, t.name),
              el("button", { class: "btn", type: "button",
                             onclick: () => setTypeArchived(t, false).catch(handleError) },
                 "Restore")))))
      : null);
}

/* ---- session ----------------------------------------------------------- */

function renderLogin() {
  renderSeq++; /* drop any page still loading */
  logoutButton.hidden = true;
  const password = el("input", { type: "password", name: "password", required: true,
                                 autocomplete: "current-password" });
  app.replaceChildren(form({ class: "card login" }, async () => {
    setStatus("Checking…");
    try {
      await api("POST", "/api/login", { password: password.value });
    } catch (err) {
      password.value = "";
      password.focus();
      setStatus("");
      throw err;
    }
    setStatus("");
    route();
  },
    el("h1", {}, "nylm"),
    field("Password", password),
    el("div", { class: "actions" }, el("button", { class: "btn go", type: "submit" }, "Log in"))));
  password.focus();
}

logoutButton.addEventListener("click", async () => {
  try {
    await api("POST", "/api/logout");
    setStatus("Logged out");
  } catch (err) {
    setStatus("Logout failed: " + err.message, true);
    return;
  }
  renderLogin();
});

/* ---- startup ----------------------------------------------------------- */

/*
 * A 401 means the session is gone: show the login form (returns true). Any
 * other error goes to the status line; the caller may also show it inline.
 */
function handleError(err) {
  if (err instanceof ApiError && err.status === 401) {
    if (!app.querySelector(".login")) {
      setStatus("Please log in");
      renderLogin();
      return true;
    }
    return false; /* wrong password: the login form shows it */
  }
  setStatus(err.message || String(err), true);
  return false;
}

async function start() {
  try {
    await api("GET", "/api/session");
    route();
  } catch (err) {
    handleError(err);
  }
}

start();
