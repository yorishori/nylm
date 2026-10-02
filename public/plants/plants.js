"use strict";

/*
 * Plants app (/plants/). Sections by URL hash:
 *   #/due            what to do, soonest first
 *   #/plants[/ID]    plants, or one plant with its care rules
 *   #/journal[/ID]   a plant's care log and notes
 *   #/types          care types
 *
 * Colour has three jobs, each with its own shape:
 *   a plant's colour      = stripe on its card's left edge, dot before its name
 *   a care type's colour  = filled chip with its name
 *   urgency               = the due label's text: rose late, peach today
 */

/* ---- due dates --------------------------------------------------------- */

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

/* Urgency colours only late and today; everything else is plain text. */
function dueTone(daysLeft) {
  if (daysLeft == null) return "paused";
  if (daysLeft < 0) return "late";
  if (daysLeft === 0) return "today";
  return "";
}

function dueLabel(daysLeft, due) {
  const tone = dueTone(daysLeft);
  return el("span", { class: tone ? "due " + tone : "due" }, dueText(daysLeft, due));
}

/* ---- identity ---------------------------------------------------------- */

/* A plant's name with its colour dot. */
function plantName(name, color, tag) {
  return el(tag || "span", { class: "plant-name" },
    el("span", { class: `dot c-${color}`, "aria-hidden": "true" }), name);
}

/* A care type as a chip in its colour; null type is a plain note. */
function careChip(name, color) {
  if (name == null) return el("span", { class: "chip note" }, "Note");
  return el("span", { class: `chip c-${color}` }, name);
}

/* A dropdown of care types shown as chips; noteOption adds "Note only" (""). */
function careTypeDropdown(label, types, selected, noteOption) {
  const options = types.map((t) => ({
    value: String(t.id), label: t.name, render: () => careChip(t.name, t.color),
  }));
  if (noteOption) options.unshift({ value: "", label: "Note only", render: () => careChip(null) });
  return dropdown({ label, options, value: selected == null ? "" : String(selected) });
}

/* ---- shell ------------------------------------------------------------- */

const TABS = [
  ["due", "Due"],
  ["plants", "Plants"],
  ["journal", "Journal"],
  ["types", "Care types"],
];

function shell(active, ...content) {
  return el("section", {},
    el("header", { class: "app-head" },
      el("h1", {}, "Plants"),
      el("nav", { class: "tabs", "aria-label": "Plants sections" },
        TABS.map(([key, label]) =>
          el("a", { href: `#/${key}`, "aria-current": key === active ? "page" : null },
             label)))),
    ...content);
}

/* Logs care done on a date, then shows the page again. */
async function logCare(plantId, plantName, typeId, typeName, date) {
  await api("POST", "/api/plants/log/add",
            { plant_id: plantId, care_type_id: typeId, date, note: "" });
  setStatus(`Logged ${typeName.toLowerCase()} for ${plantName}`);
  refresh();
}

/*
 * The two ways to mark care as done: today, or on an earlier date (which
 * opens a small form with a calendar).
 */
function doneButtons(plantId, plantName, typeId, typeName, ...extra) {
  const date = datePicker({ label: "Done on", value: today(), max: today() });
  const later = form({ hidden: true }, async () => {
    await logCare(plantId, plantName, typeId, typeName, date.value);
  },
    el("div", { class: "row" },
      field("Done on", date),
      el("button", { class: "btn go", type: "submit" }, "Log"),
      el("button", { class: "btn", type: "button",
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
      el("button", { class: "btn", type: "button", onclick: () => { later.hidden = false; } },
         "Done on a date…"),
      ...extra),
    later,
  ];
}

/* ---- due --------------------------------------------------------------- */

const DUE_GROUPS = [
  ["Late", (d) => d < 0],
  ["Today", (d) => d === 0],
  ["This week", (d) => d > 0 && d <= 7],
  ["Later", (d) => d > 7],
];

function dueRow(item) {
  return el("li", { class: `card marked due-row c-${item.plant_color}` },
    el("div", { class: "what" },
      plantName(item.plant, item.plant_color, "strong"),
      el("span", {}, careChip(item.care_type, item.care_type_color))),
    el("span", { class: "when" }, dueLabel(item.days_left, item.due)),
    el("div", { class: "done" },
      doneButtons(item.plant_id, item.plant, item.care_type_id, item.care_type,
                  navButton("Open plant", `#/plants/${item.plant_id}`))));
}

async function duePage() {
  const due = await api("GET", "/api/plants/due");
  if (due.length === 0) {
    return shell("due",
      el("div", { class: "empty" },
        el("p", {}, "Nothing is scheduled yet. Add a care rule to a plant."),
        el("div", { class: "actions" }, navButton("Go to Plants", "#/plants"))));
  }
  return shell("due",
    DUE_GROUPS.map(([title, test]) => {
      const items = due.filter((d) => test(d.days_left));
      if (items.length === 0) return null;
      return el("section", { class: "section" },
        el("header", {}, el("h2", {}, title, " ", el("span", { class: "count" }, items.length))),
        el("ul", { class: "list cols" }, items.map(dueRow)));
    }));
}

/* ---- plants ------------------------------------------------------------ */

const PLANT_LIMITS = { name: 100, species: 100, location: 100, notes: 4000 };

/* Inputs for a plant's details, filled from plant (or empty, with color). */
function plantInputs(plant, color) {
  const p = plant || {};
  return {
    color: colorPicker(p.color || color),
    name: el("input", { name: "name", required: true, maxlength: PLANT_LIMITS.name,
                        value: p.name || "" }),
    species: el("input", { name: "species", maxlength: PLANT_LIMITS.species,
                           value: p.species || "" }),
    location: el("input", { name: "location", maxlength: PLANT_LIMITS.location,
                            value: p.location || "" }),
    acquired: datePicker({ label: "Acquired", value: p.acquired || null, max: today(),
                           nullable: true }),
    notes: el("textarea", { name: "notes", maxlength: PLANT_LIMITS.notes }, p.notes || ""),
  };
}

function plantFields(inputs) {
  return [
    field("Name", inputs.name),
    el("div", { class: "row" }, field("Species", inputs.species),
       field("Location", inputs.location)),
    field("Acquired", inputs.acquired, "Leave it as Not set if you don't know."),
    field("Notes", inputs.notes),
    inputs.color,
  ];
}

function plantBody(inputs) {
  return {
    name: inputs.name.value.trim(),
    species: inputs.species.value.trim(),
    location: inputs.location.value.trim(),
    acquired: inputs.acquired.value,
    notes: inputs.notes.value,
    color: inputs.color.value,
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
  return el("li", { class: `card marked plant-card c-${plant.color}` },
    plantName(plant.name, plant.color, "h3"),
    urgent
      ? el("span", { class: "when" }, dueLabel(urgent.days_left, urgent.due))
      : el("span", { class: "when due paused" }, "Nothing scheduled"),
    urgent ? el("span", { class: "next" }, careChip(urgent.care_type, urgent.care_type_color))
           : null,
    where ? el("span", { class: "muted" }, where) : null,
    el("div", { class: "actions" }, navButton("Open", `#/plants/${plant.id}`)));
}

function addPlantForm(color) {
  const inputs = plantInputs(null, color);
  const node = form({ class: "raised", hidden: true }, async () => {
    const created = await api("POST", "/api/plants/add", plantBody(inputs));
    setStatus(`Added ${inputs.name.value.trim()}`);
    go(`#/plants/${created.id}`);
  },
    el("h2", {}, "New plant"),
    plantFields(inputs),
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "submit" }, "Add plant"),
      el("button", { class: "btn", type: "button",
                     onclick: () => { node.hidden = true; } }, "Cancel")));
  return node;
}

async function setPlantArchived(plant, archived) {
  await api("POST", "/api/plants/archive", { id: plant.id, archived });
  setStatus(`${archived ? "Archived" : "Restored"} ${plant.name}`);
  refresh();
}

async function plantsPage() {
  const [data, due] = await Promise.all([api("GET", "/api/plants"),
                                         api("GET", "/api/plants/due")]);
  const urgent = mostUrgent(due);
  const active = data.plants.filter((p) => !p.archived);
  const archived = data.plants.filter((p) => p.archived);
  const addForm = addPlantForm(nextColor(data.plants.map((p) => p.color)));

  return shell("plants",
    el("section", { class: "section" },
      el("header", {},
        el("h2", {}, "Plants ", el("span", { class: "count" }, active.length)),
        el("button", { class: "btn go", type: "button",
                       onclick: () => { addForm.hidden = false; addForm.querySelector("input").focus(); } },
           "Add plant")),
      addForm,
      active.length
        ? el("ul", { class: "list cols" }, active.map((p) => plantCard(p, urgent.get(p.id))))
        : el("p", { class: "empty" }, "No plants yet. Add your first one.")),
    archived.length
      ? el("section", { class: "section" },
          el("header", {}, el("h2", {}, "Archived ", el("span", { class: "count" }, archived.length))),
          el("ul", { class: "list cols" }, archived.map((p) =>
            el("li", { class: `card marked item c-${p.color}` },
              plantName(p.name, p.color, "strong"),
              el("div", { class: "actions" },
                navButton("Open", `#/plants/${p.id}`),
                el("button", { class: "btn", type: "button",
                               onclick: () => setPlantArchived(p, false).catch(handleError) },
                   "Restore"))))))
      : null);
}

/* ---- one plant and its rules ------------------------------------------- */

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
      el("h3", {}, careChip(rule.care_type, rule.care_type_color)),
      dueLabel(rule.days_left, rule.due)),
    el("p", {}, scheduleText(rule)),
    rule.periods.length
      ? el("ul", { class: "seasons" }, rule.periods.map((p) => el("li", {}, seasonText(p))))
      : null,
    el("p", { class: "muted" },
       rule.last_done ? `Last done ${showDate(rule.last_done)}` : "Not done yet"),
    typeArchived
      ? el("div", { class: "stack" },
          el("p", { class: "hint" }, `${rule.care_type} is archived. Restore it to log it.`),
          el("div", { class: "actions" }, navButton("Go to Care types", "#/types")))
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
          refresh();
        } catch (err) {
          handleError(err);
        }
      } }, "Delete rule")));
  return el("li", { class: "card" }, view, editor);
}

/* One editable season: a range of days of the year and its interval or "paused". */
function seasonRow(season, onRemove) {
  const s = season || { start_month: 11, start_day: 1, end_month: 2, end_day: 28,
                        interval_days: 14 };
  const from = monthDayPicker({ label: "Season starts", month: s.start_month, day: s.start_day });
  const to = monthDayPicker({ label: "Season ends", month: s.end_month, day: s.end_day });
  const days = el("input", { type: "number", min: 1, max: 3650, "aria-label": "Days",
                             value: s.interval_days || 7 });
  const daysLabel = el("span", {}, "days");
  const sync = (kind) => { days.hidden = daysLabel.hidden = kind === "paused"; };
  const kind = dropdown({
    label: "In this season",
    options: [{ value: "every", label: "every" }, { value: "paused", label: "paused" }],
    value: s.interval_days ? "every" : "paused",
    onchange: sync,
  });
  sync(kind.value);
  const row = el("div", { class: "season-row well" },
    el("span", {}, "From"), from, el("span", {}, "to"), to,
    kind, days, daysLabel,
    el("button", { class: "btn", type: "button", onclick: () => onRemove(row) },
       "Remove season"));
  row.read = () => ({
    start_month: from.value.month, start_day: from.value.day,
    end_month: to.value.month, end_day: to.value.day,
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
  const typeChoice = rule ? null : careTypeDropdown("Care type", types, types[0].id, false);

  const initial = r.yearly_month ? "yearly" : r.interval_days ? "every" : "seasons";
  const modeName = uniqueId("mode");
  const mode = (value, label) => el("label", { class: "choice" },
    el("input", { type: "radio", name: modeName, value, checked: value === initial }), label);
  const interval = el("input", { type: "number", min: 1, max: 3650,
                                 value: r.interval_days || 7, "aria-label": "Days" });
  const yearly = monthDayPicker({ label: "Once a year on", month: r.yearly_month || 3,
                                  day: r.yearly_day || 1 });

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
  const yearlyRow = el("div", { class: "row" }, el("span", {}, "On"), yearly);
  const seasonsOnly = el("p", { class: "hint" }, "Not due outside the seasons below.");
  const chosenMode = () => node.querySelector(`input[name=${modeName}]:checked`).value;

  const node = form({ class: "stack" }, async () => {
    const chosen = chosenMode();
    if (chosen === "every" && !(Number(interval.value) >= 1)) {
      throw invalid("Enter how many days between each time.");
    }
    const periods = chosen === "yearly" ? [] : [...seasonList.children].map((row) => row.read());
    if (chosen === "seasons" && !periods.some((p) => p.interval_days)) {
      throw invalid("Add a season that is not paused, or choose another schedule.");
    }
    await api("POST", "/api/plants/rules/save", {
      plant_id: plant.id,
      care_type_id: rule ? rule.care_type_id : Number(typeChoice.value),
      interval_days: chosen === "every" ? Number(interval.value) : null,
      yearly_month: chosen === "yearly" ? yearly.value.month : null,
      yearly_day: chosen === "yearly" ? yearly.value.day : null,
      periods,
    });
    setStatus("Saved the rule");
    refresh();
  },
    el("h3", {}, rule ? careChip(rule.care_type, rule.care_type_color) : "New care rule"),
    typeChoice ? field("Care type", typeChoice) : null,
    el("fieldset", { class: "stack well" },
      el("legend", { class: "hint" }, "Schedule"),
      mode("every", "Every few days"),
      mode("seasons", "Only in some seasons"),
      mode("yearly", "Once a year")),
    everyRow, seasonsOnly, yearlyRow, seasons,
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "submit" }, "Save rule"),
      el("button", { class: "btn", type: "button", onclick: onCancel }, "Cancel")));

  /* Show only the inputs of the chosen schedule. */
  const sync = () => {
    const chosen = chosenMode();
    everyRow.hidden = chosen !== "every";
    seasonsOnly.hidden = chosen !== "seasons";
    yearlyRow.hidden = chosen !== "yearly";
    seasons.hidden = chosen === "yearly";
  };
  node.addEventListener("change", (e) => { if (e.target.name === modeName) sync(); });
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
  const details = form({ class: "raised" }, async () => {
    await api("POST", "/api/plants/update", { id: plant.id, ...plantBody(inputs) });
    setStatus(`Saved ${inputs.name.value.trim()}`);
    refresh();
  },
    plantFields(inputs),
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "submit" }, "Save details")));

  return shell("plants",
    el("div", { class: "actions" }, navButton("All plants", "#/plants")),
    el("header", { class: "section" },
      plantName(plant.name, plant.color, "h2"),
      plant.archived ? el("p", { class: "muted" }, "Archived: not shown in Due.") : null),

    el("div", { class: "split" },
      el("section", { class: "section" },
        el("header", {},
          el("h2", {}, "Care rules ", el("span", { class: "count" }, plant.rules.length)),
          freeTypes.length
            ? el("button", { class: "btn go", type: "button", onclick: () => {
                addRule.replaceChildren(el("div", { class: "raised" },
                  ruleForm(plant, null, freeTypes, () => { addRule.hidden = true; })));
                addRule.hidden = false;
              } }, "Add care rule")
            : null),
        freeTypes.length === 0
          ? el("div", { class: "stack" },
              el("p", { class: "hint" }, "Every care type has a rule here."),
              el("div", { class: "actions" }, navButton("Add care types", "#/types")))
          : null,
        addRule,
        plant.rules.length
          ? el("ul", { class: "list" }, plant.rules.map((r) => ruleCard(plant, r, data.care_types)))
          : el("p", { class: "empty" }, "No care rules yet.")),

      el("div", { class: "side" },
        el("section", { class: "section" },
          el("header", {},
            el("h2", {}, "Journal"),
            navButton("Open journal", `#/journal/${plant.id}`))),

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
                   "Archive plant"))))));
}

/* ---- journal ----------------------------------------------------------- */

/* Inputs for a log entry, filled from entry (or today / note only). */
function entryInputs(entry, types) {
  const e = entry || { date: today(), care_type_id: null, note: "" };
  return {
    date: datePicker({ label: "Date", value: e.date, max: today() }),
    type: careTypeDropdown("What", types, e.care_type_id, true),
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

function entryCard(entry, typeById, types) {
  const type = entry.care_type_id == null ? null : typeById.get(entry.care_type_id);
  /* An entry may keep an archived type, so offer it while editing. */
  const editTypes = types.filter((t) => !t.archived || t.id === entry.care_type_id);
  const editor = el("div", { hidden: true });
  const view = el("div", { class: "entry" },
    el("header", {},
      el("strong", {}, showDate(entry.date)),
      type ? careChip(type.name, type.color) : careChip(null)),
    entry.note ? el("p", { class: "note" }, entry.note) : null,
    el("div", { class: "actions" },
      el("button", { class: "btn", type: "button", onclick: () => {
        const inputs = entryInputs(entry, editTypes);
        editor.replaceChildren(form({}, async () => {
          await api("POST", "/api/plants/log/update", { id: entry.id, ...entryBody(inputs) });
          setStatus("Saved the entry");
          refresh();
        },
          entryFields(inputs),
          el("div", { class: "actions" },
            el("button", { class: "btn go", type: "submit" }, "Save entry"),
            el("button", { class: "btn", type: "button", onclick: () => {
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
          refresh();
        } catch (err) {
          handleError(err);
        }
      } }, "Delete")));
  const li = el("li", { class: "card" }, view, editor);
  li.entryId = entry.id; /* for paging */
  return li;
}

async function journalPage(id) {
  const data = await api("GET", "/api/plants");
  const plants = data.plants;
  if (plants.length === 0) {
    return shell("journal",
      el("div", { class: "empty" },
        el("p", {}, "Add a plant to start its journal."),
        el("div", { class: "actions" }, navButton("Go to Plants", "#/plants"))));
  }
  const plant = plants.find((p) => p.id === id) ||
                plants.find((p) => !p.archived) || plants[0];
  if (plant.id !== id) {
    history.replaceState(null, "", `#/journal/${plant.id}`);
  }

  const log = await api("GET", `/api/plants/log?plant_id=${plant.id}`);
  const types = data.care_types;
  const typeById = new Map(types.map((t) => [t.id, t]));
  const active = types.filter((t) => !t.archived);

  const picker = dropdown({
    label: "Show another plant",
    options: plants.map((p) => ({
      value: String(p.id),
      label: p.archived ? `${p.name} (archived)` : p.name,
      render: () => plantName(p.archived ? `${p.name} (archived)` : p.name, p.color),
    })),
    value: String(plant.id),
    onchange: (value) => go(`#/journal/${value}`),
  });

  const inputs = entryInputs(null, active);
  const add = form({ class: "raised" }, async () => {
    await api("POST", "/api/plants/log/add", { plant_id: plant.id, ...entryBody(inputs) });
    setStatus("Added the entry");
    refresh();
  },
    el("h2", {}, "New entry"),
    entryFields(inputs),
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "submit" }, "Add entry")));

  const list = el("ul", { class: "list" }, log.entries.map((e) => entryCard(e, typeById, types)));
  const olderButton = el("button", { class: "btn", type: "button", onclick: async () => {
    olderButton.disabled = true;
    try {
      const last = list.lastElementChild.entryId;
      const older = await api("GET", `/api/plants/log?plant_id=${plant.id}&before=${last}`);
      for (const e of older.entries) list.append(entryCard(e, typeById, types));
      olderButton.hidden = !older.more;
    } catch (err) {
      handleError(err);
    } finally {
      olderButton.disabled = false;
    }
  } }, "Show older entries");
  olderButton.hidden = !log.more;

  return shell("journal",
    el("div", { class: `card marked plant-picker c-${plant.color}` },
      plantName(plant.name, plant.color, "h2"),
      field("Show another plant", picker),
      navButton("Open plant", `#/plants/${plant.id}`)),
    el("div", { class: "split" },
      el("section", { class: "section" },
        el("header", {}, el("h2", {}, "Entries")),
        log.entries.length ? list : el("p", { class: "empty" }, "Nothing written yet."),
        el("div", { class: "actions" }, olderButton)),
      el("div", { class: "side first" }, add)));
}

/* ---- care types -------------------------------------------------------- */

async function setTypeArchived(type, archived) {
  await api("POST", "/api/plants/types/archive", { id: type.id, archived });
  setStatus(`${archived ? "Archived" : "Restored"} ${type.name}`);
  refresh();
}

function typeRow(type) {
  const name = el("input", { value: type.name, required: true, maxlength: 50,
                             "aria-label": "Name" });
  const color = colorPicker(type.color);
  color.hidden = true;
  const showColors = el("button", { class: "btn", type: "button", onclick: () => {
    color.hidden = false;
    showColors.hidden = true;
  } }, "Change colour");
  return el("li", { class: "card" },
    form({}, async () => {
      await api("POST", "/api/plants/types/update",
                { id: type.id, name: name.value.trim(), color: color.value });
      setStatus(`Saved ${name.value.trim()}`);
      refresh();
    },
      careChip(type.name, type.color),
      name,
      color,
      el("div", { class: "actions" },
        el("button", { class: "btn", type: "submit" }, "Save"),
        showColors,
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
  const color = colorPicker(nextColor(data.care_types.map((t) => t.color)));

  return shell("types",
    el("section", { class: "section" },
      el("p", { class: "hint" },
         "Care types are what you do to plants. Each plant gets its own rule per type."),
      form({ class: "raised" }, async () => {
        await api("POST", "/api/plants/types/add",
                  { name: name.value.trim(), color: color.value });
        setStatus(`Added ${name.value.trim()}`);
        refresh();
      },
        name,
        color,
        el("div", { class: "actions" },
          el("button", { class: "btn go", type: "submit" }, "Add care type")))),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Care types ", el("span", { class: "count" }, active.length))),
      active.length
        ? el("ul", { class: "list cols" }, active.map(typeRow))
        : el("p", { class: "empty" }, "No care types yet.")),
    archived.length
      ? el("section", { class: "section" },
          el("header", {}, el("h2", {}, "Archived ", el("span", { class: "count" }, archived.length))),
          el("ul", { class: "list cols" }, archived.map((t) =>
            el("li", { class: "card item" },
              careChip(t.name, t.color),
              el("div", { class: "actions" },
                el("button", { class: "btn", type: "button",
                               onclick: () => setTypeArchived(t, false).catch(handleError) },
                   "Restore"))))))
      : null);
}

/* ---- routing ----------------------------------------------------------- */

/* ["plants", "3"] -> the plant page, etc. Unknown hashes go to Due. */
function route(parts) {
  const [section, rawId] = parts;
  const id = rawId === undefined ? null : Number(rawId);
  if (id !== null && !(Number.isInteger(id) && id > 0)) {
    go("#/due");
    return null;
  }
  if (section === "due" && id === null) return duePage();
  if (section === "plants") return id ? plantPage(id) : plantsPage();
  if (section === "journal") return journalPage(id);
  if (section === "types" && id === null) return typesPage();
  go("#/due");
  return null;
}

startApp(route);
