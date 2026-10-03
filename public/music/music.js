"use strict";

/*
 * Music app (/music/). Sections by URL hash:
 *   #/albums       every album (tracks sharing ALBUM and ALBUMARTIST) in an
 *                  editable table with filters, and the library scan
 *   #/album/ID     the tracks of the album of track ID, every tag editable
 *   #/changes      the queued changes by batch: discard them, write them
 *   #/info         library counts, the rules, the written changes
 *
 * The server never touches the files: it keeps a cache of their tags and a
 * queue of changes. Every edit here is queued as soon as it is made (no
 * save button): one batch per edit. The write service puts the queue into
 * the files, the scan reads the files into the cache; both are started
 * from this page with the password again. While one runs, nothing can be
 * queued. What the pages show is planned: the files' values with the
 * queued changes applied.
 */

/* Every tag the pages show: label, kind of editor, whether required. */
const FIELDS = {
  discnumber: { label: "Disc", kind: "number", required: true },
  tracknumber: { label: "Track", kind: "number", required: true },
  title: { label: "Title", kind: "text", required: true },
  artist: { label: "Artist", kind: "text", required: true },
  album: { label: "Album", kind: "text", required: true },
  albumartist: { label: "Album artist", kind: "combo", required: true },
  date: { label: "Date", kind: "whole", required: true },
  composer: { label: "Composer", kind: "list", required: true },
  genre: { label: "Genres", kind: "list", required: true },
  compilation: { label: "Compilation", kind: "switch", required: true },
  bpm: { label: "BPM", kind: "whole" },
  isrc: { label: "ISRC", kind: "text" },
  asin: { label: "ASIN", kind: "text" },
  copyright: { label: "Copyright", kind: "text" },
  encodedby: { label: "Encoded by", kind: "text" },
  mood: { label: "Mood", kind: "text" },
  media: { label: "Media", kind: "text" },
  label: { label: "Label", kind: "text" },
  catalognumber: { label: "Catalog no.", kind: "text" },
  barcode: { label: "Barcode", kind: "text" },
  musicbrainz_trackid: { label: "MusicBrainz track", kind: "text" },
  musicbrainz_albumid: { label: "MusicBrainz album", kind: "text" },
};
/* The albums table's columns; also the tags an album's tracks share. */
const ALBUM_COLUMNS = ["album", "albumartist", "date", "composer", "genre", "compilation"];
/* The album page's tag columns (the sort tags and Navidrome id are not shown). */
const TRACK_COLUMNS = Object.keys(FIELDS);

const MAX_BYTES = 500;
const MAX_VALUES = 64;
const MAX_SUGGESTIONS = 50;
const SHOW_STEP = 200; /* albums shown at a time */
const POLL_MS = 3000;
const GENRE_RE = /^[a-z0-9-]+$/;
const WHOLE_RE = /^(?!0+$)\d{1,4}$/; /* a positive whole number, at most 9999 */
const encoder = new TextEncoder();

/* ---- values -------------------------------------------------------------- */

/* "X/Y": two positive whole numbers, X at most Y. */
function numberValid(s) {
  const m = /^(\d{1,4})\/(\d{1,4})$/.exec(s);
  return Boolean(m) && Number(m[1]) >= 1 && Number(m[1]) <= Number(m[2]);
}

/* The X of "X/Y" or "X" (Y is a separate number); null if there is none. */
function firstNumber(s) {
  const m = /^(\d{1,4})(\/\d*)?$/.exec(s || "");
  return m && Number(m[1]) >= 1 ? Number(m[1]) : null;
}

function textProblem(s) {
  if (encoder.encode(s).length > MAX_BYTES) return "is too long (at most 500 bytes)";
  if (/[\u0000-\u001f\u007f]/.test(s)) return "can not contain control characters";
  return null;
}

/* What is wrong with a new value (a string, or a list for genre and
 * composer), or null. The server checks the same rules. */
function problem(field, v) {
  const spec = FIELDS[field];
  if (spec.kind === "list") {
    if (!v.length) return "needs at least one value";
    if (v.length > MAX_VALUES) return "has too many values (at most 64)";
    for (const x of v) {
      if (x === "") return "can not have an empty value";
      const p = textProblem(x);
      if (p) return p;
      if (field === "genre" && !GENRE_RE.test(x)) return `"${x}": only lowercase a-z, 0-9 and -`;
    }
    return null;
  }
  if (v === "") return spec.required ? "is required" : null;
  const p = textProblem(v);
  if (p) return p;
  if (spec.kind === "whole" && !WHOLE_RE.test(v)) {
    return field === "date" ? "must be a year: a positive whole number" : "must be a positive whole number";
  }
  if (spec.kind === "number" && !numberValid(v)) return "must be like 3/12 (the first at most the second)";
  if (field === "compilation" && v !== "0" && v !== "1") return "must be 0 or 1";
  return null;
}

/* A value for people. */
function showValue(field, v) {
  if (v == null || (Array.isArray(v) && !v.length)) return "—";
  if (field === "compilation") return v === "1" ? "yes" : "no";
  return Array.isArray(v) ? v.join("; ") : v;
}

/* A value as the changes table stores it (genre and composer as JSON). */
function fromStored(field, s) {
  if (s == null) return null;
  if (FIELDS[field] && FIELDS[field].kind === "list") {
    try {
      return JSON.parse(s);
    } catch (err) {
      return s;
    }
  }
  return s;
}

function sameValue(a, b) {
  return JSON.stringify(a ?? null) === JSON.stringify(b ?? null);
}

/* "3.2 MB" */
function showSize(bytes) {
  const units = ["B", "KB", "MB", "GB", "TB"];
  let v = bytes;
  let i = 0;
  while (v >= 1000 && i < units.length - 1) {
    v /= 1000;
    i++;
  }
  return `${i ? v.toFixed(1) : v} ${units[i]}`;
}

/* A unix time as "Sat 3 Oct 14:05". */
function showTime(ts) {
  if (!ts) return "—";
  const d = new Date(ts * 1000);
  return `${showDate(isoDate(d.getFullYear(), d.getMonth() + 1, d.getDate()))} ` +
         `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
}

/* A path relative to the music folder. */
let musicRoot = "";
function relative(path) {
  if (path == null) return "(no longer in the library)";
  return musicRoot && path.startsWith(musicRoot + "/") ? path.slice(musicRoot.length + 1) : path;
}

/* ---- shell --------------------------------------------------------------- */

const tabLabels = {};

function changesLabel(pending) {
  return pending ? `Changes (${pending})` : "Changes";
}

function shell(active, overview, ...content) {
  musicRoot = overview.root || "";
  const tabs = [["albums", "Albums"], ["changes", changesLabel(overview.pending)], ["info", "Info"]];
  return el("section", {},
    el("header", { class: "app-head" },
      el("h1", {}, "Music"),
      el("nav", { class: "tabs", "aria-label": "Music sections" },
        tabs.map(([key, label]) => {
          const a = el("a", { href: `#/${key}`, "aria-current": key === active ? "page" : null },
                       label);
          tabLabels[key] = a;
          return a;
        }))),
    ...content);
}

/* Updates the Changes tab's count after a change was queued. */
async function updatePendingCount() {
  const o = await api("GET", "/api/music");
  if (tabLabels.changes && tabLabels.changes.isConnected) {
    tabLabels.changes.textContent = changesLabel(o.pending);
  }
}

/* Queues changes (one batch); the suggestions are fetched again after. */
async function queue(body) {
  const result = await api("POST", "/api/music/queue", body);
  valueCache.clear();
  updatePendingCount().catch(handleError);
  if (result.dropped && !result.queued) setStatus("Back to the files' value");
  else if (result.queued) setStatus(`Queued: ${plural(result.queued, "change", "changes")}`);
  return result;
}

/* ---- services ------------------------------------------------------------ */

function busyText(o) {
  if (o.busy === "scan") {
    const r = o.running;
    return r ? `Scanning ${r.path ? relative(r.path) : "the library"}… ` +
               `${plural(r.files, "file", "files")} so far.`
             : "Scanning…";
  }
  if (o.busy === "write") return "Writing changes to the files…";
  return o.busy ? "Starting…" : "";
}

/*
 * While a service runs, checks back every few seconds: text shows the
 * progress; when it ends the page is shown again. Stops when node is no
 * longer on the page.
 */
function watchBusy(o, node, text) {
  if (!o.busy) return;
  const poll = async () => {
    if (!node.isConnected) return;
    try {
      const now = await api("GET", "/api/music");
      if (!node.isConnected) return;
      if (now.busy) {
        text.textContent = busyText(now);
        setTimeout(poll, POLL_MS);
      } else {
        setStatus(o.busy === "write" ? "Writing finished: see the results in Info"
                                     : "Scan finished");
        refresh();
      }
    } catch (err) {
      handleError(err);
    }
  };
  setTimeout(poll, POLL_MS);
}

/* A password form that starts a service (POST path with extra), then shows
 * the page again. */
function serviceForm(path, extra, intro, submitLabel, started) {
  const password = el("input", { type: "password", name: "password", required: true,
                                 autocomplete: "current-password" });
  const node = form({ class: "raised", hidden: true }, async () => {
    try {
      await api("POST", path, { ...extra, password: password.value });
    } finally {
      password.value = "";
    }
    setStatus(started);
    refresh();
  },
    el("p", {}, intro),
    field("Password", password, "Starting it needs your password again."),
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "submit" }, submitLabel),
      el("button", { class: "btn", type: "button", onclick: () => { node.hidden = true; } },
         "Cancel")));
  node.open = () => {
    node.hidden = false;
    password.focus();
  };
  return node;
}

const SCAN_STATE = {
  done: "done",
  incomplete: "incomplete: something could not be read, so nothing was removed (see the server log)",
  failed: "failed (see the server log)",
  running: "did not finish",
};

function scanSummary(s) {
  if (!s) return "Never scanned.";
  return `Last scan ${showTime(s.finished || s.started)}, ${SCAN_STATE[s.state] || s.state}: ` +
         `${plural(s.files, "music file", "music files")}, ${s.parsed} read, ` +
         `${s.removed} removed${s.failed ? `, ${s.failed} could not be read` : ""}.`;
}

function othersText(s) {
  if (!s || !s.others || !s.others.length) return "No other files.";
  return "Other files: " + s.others.map((x) => `${x.ext} ${x.count}`).join(", ") + ".";
}

/* The folder, the last scan and the scan button. */
function scanCard(o) {
  const text = el("p", {}, o.busy ? busyText(o) : scanSummary(o.scan));
  const scan = serviceForm("/api/music/scan", {},
    "The scan reads new and changed files into nylm. It changes no file.",
    "Start scan", "Scan started");
  const card = el("section", { class: "card stack" },
    el("h2", {}, "Library"),
    el("p", { class: "path muted" }, o.root),
    o.available ? null
                : el("p", { class: "warn" },
                     "The music folder is not available (is the drive mounted?). The albums " +
                     "below are from the last scan; nothing can be scanned or written now."),
    text,
    o.busy ? null : el("p", { class: "muted" }, othersText(o.scan)),
    o.busy || !o.available ? null
      : el("div", { class: "actions" },
          el("button", { class: "btn", type: "button", onclick: () => scan.open() },
             "Scan again…")),
    scan);
  watchBusy(o, card, text);
  return card;
}

/* ---- editors ------------------------------------------------------------- */

/* The values in use for a field, fetched once until something is queued. */
const valueCache = new Map();
function knownValues(field) {
  if (!valueCache.has(field)) {
    valueCache.set(field, api("GET", `/api/music/values?field=${field}`).catch((err) => {
      valueCache.delete(field);
      throw err;
    }));
  }
  return valueCache.get(field);
}

/*
 * A popup under anchor (fixed, so a scrolling table does not cut it off)
 * that closes on Escape or a press outside. onClose(refocus) is called
 * once. Returns {pop, close}.
 */
function floatingPopup(anchor, label, onClose) {
  const pop = el("div", { class: "popover floating", role: "dialog", "aria-label": label });
  const owner = { contains: (n) => pop.contains(n) || anchor.contains(n) };
  let closed = false;
  const close = (refocus) => {
    if (closed) return;
    closed = true;
    pop.remove();
    popoverClosed(owner);
    onClose(refocus);
  };
  document.body.append(pop);
  if (!window.matchMedia("(max-width: 21rem)").matches) {
    /* Positioned with the CSS object model: not an inline style attribute. */
    const r = anchor.getBoundingClientRect();
    pop.style.top = `${Math.round(r.bottom + 6)}px`;
    pop.style.left = `${Math.round(Math.max(8, Math.min(r.left,
                                                        window.innerWidth - 8 - pop.offsetWidth)))}px`;
  }
  popoverOpened(owner, pop, close);
  return { pop, close };
}

/*
 * A text input with suggestions from the library's values of field
 * (combobox). onPick(value) when one is chosen with Enter or a press;
 * exclude() lists values not to suggest. Returns {input, list}.
 */
function suggestInput(field, label, onPick, exclude) {
  const input = el("input", { placeholder: "Type or choose…", "aria-label": label,
                              role: "combobox", "aria-autocomplete": "list",
                              "aria-expanded": "false" });
  const listId = uniqueId("suggest");
  const list = el("ul", { class: "listbox suggestions", role: "listbox", id: listId,
                          "aria-label": "Suggestions" });
  input.setAttribute("aria-controls", listId);
  let known = [];
  let shown = [];
  let active = -1;

  function render() {
    const q = input.value.toLowerCase();
    const skip = exclude ? exclude() : [];
    shown = known.filter((v) => !skip.includes(v) && (!q || v.toLowerCase().includes(q)))
                 .slice(0, MAX_SUGGESTIONS);
    active = Math.min(active, shown.length - 1);
    list.replaceChildren(...shown.map((v, i) => el("li", {
      role: "option", id: `${listId}-${i}`, class: i === active ? "option active" : "option",
      "aria-selected": String(i === active),
      onpointerdown: (e) => e.preventDefault(), /* keep the focus in the input */
      onclick: () => onPick(v),
    }, v)));
    input.setAttribute("aria-expanded", String(shown.length > 0));
    if (active >= 0) input.setAttribute("aria-activedescendant", `${listId}-${active}`);
    else input.removeAttribute("aria-activedescendant");
  }
  input.addEventListener("input", () => { active = -1; render(); });
  input.addEventListener("keydown", (e) => {
    if (e.key === "ArrowDown" && shown.length) active = Math.min(active + 1, shown.length - 1);
    else if (e.key === "ArrowUp" && shown.length) active = Math.max(active - 1, -1);
    else if (e.key === "Enter") onPick(active >= 0 ? shown[active] : input.value);
    else return;
    e.preventDefault();
    render();
  });
  input.refresh = () => { active = -1; render(); };
  knownValues(field).then((values) => {
    known = values;
    render();
  }).catch(handleError);
  return { input, list };
}

/* Save and Cancel for a popup. */
function popupActions(save, close) {
  return el("div", { class: "actions" },
    el("button", { class: "btn go", type: "button", onclick: save }, "Save"),
    el("button", { class: "btn", type: "button", onclick: () => close(true) }, "Cancel"));
}

/* One value from the library's values or typed in (album artist).
 * onSave(value) resolves true when queued. */
function comboEditor(anchor, { field, value, onSave, onDone }) {
  const spec = FIELDS[field];
  const { pop, close } = floatingPopup(anchor, spec.label, onDone);
  const error = el("p", { class: "form-error", role: "alert" });
  const save = async (v) => {
    const why = problem(field, v);
    if (why) {
      error.textContent = `${spec.label} ${why}.`;
      return;
    }
    if (v === value || (await onSave(v))) close(true);
  };
  const { input, list } = suggestInput(field, spec.label, save);
  input.value = value ?? "";
  pop.append(el("strong", {}, spec.label), input, list, error,
             popupActions(() => save(input.value), close));
  input.focus();
  input.select();
}

/*
 * An ordered list of values (genres, composers): reorder, remove, add from
 * the library's values or typed in. onSave(list) resolves true when queued.
 */
function listEditor(anchor, { field, values, mixed, onSave, onDone }) {
  const spec = FIELDS[field];
  let chosen = [...values];
  let touched = false;
  const { pop, close } = floatingPopup(anchor, spec.label, onDone);
  const items = el("ol", { class: "ordered", "aria-label": `${spec.label} in order` });
  const error = el("p", { class: "form-error", role: "alert" });

  function move(i, by) {
    const [v] = chosen.splice(i, 1);
    chosen.splice(i + by, 0, v);
    touched = true;
    render();
  }
  function render() {
    items.replaceChildren(...chosen.map((v, i) => el("li", {},
      el("span", { class: "value" }, v),
      el("button", { type: "button", class: "btn mini", "aria-label": `Move ${v} up`,
                     disabled: i === 0, onclick: () => move(i, -1) }, "↑"),
      el("button", { type: "button", class: "btn mini", "aria-label": `Move ${v} down`,
                     disabled: i === chosen.length - 1, onclick: () => move(i, 1) }, "↓"),
      el("button", { type: "button", class: "btn mini danger", "aria-label": `Remove ${v}`,
                     onclick: () => { chosen.splice(i, 1); touched = true; render(); } }, "×"))));
    if (!chosen.length) {
      items.append(el("li", { class: "muted" }, mixed && !touched ? "Differs between tracks" : "None"));
    }
    if (input.refresh) input.refresh();
  }
  const add = (v) => {
    const why = v === "" ? "Type a value first." : problem(field, [...chosen, v]);
    if (why) {
      error.textContent = v === "" ? why : `${spec.label} ${why}.`;
      return;
    }
    error.textContent = "";
    if (!chosen.includes(v)) chosen.push(v);
    touched = true;
    input.value = "";
    render();
    input.focus();
  };
  const { input, list } = suggestInput(field, `Add ${spec.label.toLowerCase()}`, add,
                                       () => chosen);
  const save = async () => {
    if (!touched) {
      close(true);
      return;
    }
    const why = problem(field, chosen);
    if (why) {
      error.textContent = `${spec.label} ${why}.`;
      return;
    }
    if (await onSave(chosen)) close(true);
  };
  pop.append(el("strong", {}, spec.label), items, input, list,
    field === "genre" ? el("p", { class: "hint" }, "Lowercase a-z, 0-9 and -.") : null,
    mixed ? el("p", { class: "hint" }, "The tracks differ: the list saved here goes on every track.") : null,
    error, popupActions(save, close));
  render();
  input.focus();
}

/* Disc or track number X/Y: X first, Y worked out but changeable.
 * onSave(x, y) resolves true when queued. */
function numberEditor(anchor, { field, x, y, onSave, onDone }) {
  const spec = FIELDS[field];
  const { pop, close } = floatingPopup(anchor, spec.label, onDone);
  const number = (value, label) => el("input", { type: "number", min: "1", max: "9999", step: "1",
                                                 inputmode: "numeric", value, "aria-label": label });
  const xs = number(x ?? "", spec.label);
  const ys = number(y, `${spec.label}s in all`);
  const error = el("p", { class: "form-error", role: "alert" });
  const save = async () => {
    const a = xs.value.trim();
    const b = ys.value.trim();
    if (!WHOLE_RE.test(a) || !WHOLE_RE.test(b)) {
      error.textContent = "Both must be whole numbers from 1 to 9999.";
      return;
    }
    if (Number(a) > Number(b)) {
      error.textContent = `${spec.label} ${a} of ${b}: the first can not be the larger.`;
      return;
    }
    if (await onSave(Number(a), Number(b))) close(true);
  };
  for (const input of [xs, ys]) {
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        e.preventDefault();
        save();
      }
    });
  }
  pop.append(el("strong", {}, spec.label),
    el("div", { class: "number-pair" }, xs, el("span", {}, "/"), ys),
    el("p", { class: "hint" },
       field === "discnumber" ? "Every track of the album gets the same number of discs; a " +
                                "track without a disc number gets disc 1."
                              : "Every track on this disc gets the same total; tracks without a " +
                                "number get the free ones."),
    error, popupActions(save, close));
  xs.focus();
  xs.select();
}

/* A switch (on / off). onToggle(on) resolves true when done. */
function switchButton({ label, on, mixed, disabled, showLabel, onToggle }) {
  const button = el("button", {
    type: "button", class: "switch", role: "switch", disabled,
    "aria-checked": mixed ? "mixed" : String(Boolean(on)),
    "aria-label": showLabel ? null : label,
  }, el("span", { class: "track", "aria-hidden": "true" }, el("span", { class: "knob" })),
     showLabel ? el("span", {}, label) : null);
  button.addEventListener("click", async () => {
    const next = button.getAttribute("aria-checked") !== "true";
    button.disabled = true;
    try {
      if (await onToggle(next)) button.setAttribute("aria-checked", String(next));
    } finally {
      button.disabled = Boolean(disabled);
    }
  });
  return button;
}

/* Runs save(value); shows its error and resolves false if it fails. */
async function attempt(label, save, value) {
  try {
    await save(value);
    return true;
  } catch (err) {
    if (!(err instanceof ApiError && err.status === 401 && handleError(err))) {
      setStatus(`${label}: ${err.message}`, true);
    }
    return false;
  }
}

/*
 * A table cell that edits one tag. value: the planned value (string, list
 * or null); mixed: tracks differ; marks: extra classes ("edited", ...);
 * key: to find the control again after the row is drawn anew; save(value)
 * queues it; number: {x, y, save(x, y)} for disc and track numbers.
 */
function editCell({ field, value, mixed, marks, key, disabled, save, number, summary }) {
  const spec = FIELDS[field];
  const td = el("td", { class: [`col-${field}`, ...(marks || [])].join(" ") });
  const label = spec.label;
  const run = (v) => attempt(label, save, v);
  const shown = mixed ? "mixed" : summary ? summary(value) : showValue(field, value);

  if (spec.kind === "text" || spec.kind === "whole") {
    const whole = spec.kind === "whole";
    const input = el("input", {
      class: "cell-input", "data-key": key, disabled, value: mixed ? "" : value ?? "",
      placeholder: mixed ? "mixed" : value == null ? "missing" : "", "aria-label": label,
      ...(whole ? { type: "number", min: "1", max: "9999", step: "1", inputmode: "numeric" } : {}),
    });
    const initial = input.value;
    input.addEventListener("keydown", (e) => {
      if (e.key === "Escape") {
        input.value = initial;
        input.blur();
      }
    });
    input.addEventListener("change", async () => {
      const v = input.value;
      if (mixed && v === "") return; /* nothing typed for differing tracks: keep each */
      const why = problem(field, v);
      if (why) {
        setStatus(`${label} ${why}.`, true);
        input.value = initial;
        return;
      }
      input.disabled = true;
      if (!(await run(v))) {
        input.value = initial;
        input.disabled = false;
      }
    });
    td.append(input);
    return td;
  }
  if (spec.kind === "switch") {
    td.append(switchButton({ label, on: value === "1", mixed, disabled,
                             onToggle: (on) => run(on ? "1" : "0") }));
    td.firstChild.setAttribute("data-key", key);
    return td;
  }
  const absent = value == null || (Array.isArray(value) && !value.length);
  const button = el("button", {
    type: "button", class: absent || mixed ? "cell empty" : "cell", "data-key": key,
    disabled, "aria-label": `${label}: ${absent && !mixed ? "missing" : shown}`,
  }, absent && !mixed ? "missing" : shown);
  let open = false;
  button.addEventListener("click", () => {
    if (open) return;
    open = true;
    const onDone = (refocus) => {
      open = false;
      if (refocus && button.isConnected) button.focus();
    };
    if (spec.kind === "list") {
      listEditor(button, { field, values: mixed ? [] : value || [], mixed, onSave: run, onDone });
    } else if (spec.kind === "combo") {
      comboEditor(button, { field, value: mixed ? "" : value, onSave: run, onDone });
    } else {
      numberEditor(button, { field, x: number.x, y: number.y,
                             onSave: (x, y) => attempt(label, () => number.save(x, y)), onDone });
    }
  });
  td.append(button);
  return td;
}

/* Focuses the control with this key again after its row was drawn anew,
 * unless the focus has moved on to something else meanwhile. */
function refocus(container, key) {
  const now = document.activeElement;
  if (now && now !== document.body && !container.contains(now)) return;
  const c = container.querySelector(`[data-key="${CSS.escape(key)}"]`);
  if (c) c.focus();
}

/* ---- albums -------------------------------------------------------------- */

/* The albums table's filters; kept while the page is drawn again. */
const filters = { q: "", album: "", albumartist: "", date: "", composer: "", genre: "",
                  invalid: false, missing: false, artists: false, noArt: false };

/* Composers in the albums table: the first, and how many more. */
function composerSummary(v) {
  if (!v || !v.length) return "—";
  return v.length > 1 ? `${v[0]} +${v.length - 1}` : v[0];
}

function albumText(a, f) {
  return showValue(f, a[f]).toLowerCase();
}

function albumMatches(a) {
  const q = filters.q.trim().toLowerCase();
  if (q && !ALBUM_COLUMNS.some((f) => albumText(a, f).includes(q))) return false;
  for (const f of ["album", "albumartist", "date", "composer", "genre"]) {
    const v = filters[f].trim().toLowerCase();
    if (v && !albumText(a, f).includes(v)) return false;
  }
  return !(filters.invalid && !a.invalid) && !(filters.missing && !a.missing) &&
         !(filters.artists && !a.several_artists) && !(filters.noArt && !a.no_art);
}

/* One album as a table row, editable unless readOnly. */
function albumRow(a, readOnly) {
  const tr = el("tr", { class: a.changed.length ? "pending" : "" });
  const save = (f) => async (value) => {
    await queue({ album: a.track, set: { [f]: value } });
    const [row] = await api("GET", `/api/music/albums?track=${a.track}`);
    Object.assign(a, row);
    const next = albumRow(a, readOnly);
    tr.replaceWith(next);
    refocus(next, `${a.track}:${f}`);
  };
  tr.append(
    ...ALBUM_COLUMNS.map((f) => editCell({
      field: f,
      value: a[f],
      mixed: a.mixed.includes(f),
      marks: a.changed.includes(f) ? ["edited"] : [],
      key: `${a.track}:${f}`,
      disabled: readOnly,
      save: save(f),
      summary: f === "composer" ? composerSummary : null,
    })),
    el("td", { class: "col-tracks" },
      navButton(String(a.tracks), `#/album/${a.track}`, "count-btn")));
  tr.lastChild.firstChild.setAttribute("aria-label", `${plural(a.tracks, "track", "tracks")}: open the album`);
  return tr;
}

function filterBar(show) {
  const text = (key, label) => {
    const input = el("input", { type: "search", value: filters[key], "aria-label": label,
                                placeholder: label });
    input.addEventListener("input", () => {
      filters[key] = input.value;
      show();
    });
    return input;
  };
  const toggle = (key, label) => switchButton({ label, on: filters[key], showLabel: true,
                                                onToggle: (on) => {
                                                  filters[key] = on;
                                                  show();
                                                  return true;
                                                } });
  return el("div", { class: "filters stack" },
    text("q", "Search every column"),
    el("div", { class: "filter-fields" },
      text("album", "Album"), text("albumartist", "Album artist"), text("date", "Date"),
      text("composer", "Composer"), text("genre", "Genre")),
    el("div", { class: "filter-switches" },
      toggle("invalid", "Invalid tags"),
      toggle("missing", "Missing tags"),
      toggle("artists", "Several artists, not a compilation"),
      toggle("noArt", "Tracks without art")));
}

function legend() {
  return el("p", { class: "legend hint" },
    el("span", { class: "mark edited" }, "Changed, not written yet"),
    el("span", { class: "mark differs" }, "Differs from the album"),
    el("span", { class: "mark bad" }, "Missing or invalid"),
    " Changes are queued as you edit; write them from Changes.");
}

async function albumsPage() {
  const o = await api("GET", "/api/music");
  if (!o.configured) {
    return el("section", {},
      el("header", { class: "app-head" }, el("h1", {}, "Music")),
      el("div", { class: "empty" },
        el("p", {}, "Music is not set up. Set NYLM_MUSIC in /etc/nylm.conf to the music " +
                    "folder, restart nylm, then scan the library.")));
  }
  const albums = await api("GET", "/api/music/albums");
  const readOnly = Boolean(o.busy);
  const body = el("tbody");
  const count = el("span", { class: "count" });
  const more = el("button", { class: "btn", type: "button" }, "Show more");
  let limit = SHOW_STEP;
  const show = () => {
    const shown = albums.filter(albumMatches);
    count.textContent = shown.length === albums.length ? albums.length
                                                       : `${shown.length} of ${albums.length}`;
    body.replaceChildren(...shown.slice(0, limit).map((a) => albumRow(a, readOnly)));
    more.hidden = shown.length <= limit;
  };
  more.addEventListener("click", () => {
    limit += SHOW_STEP;
    show();
  });
  const table = el("div", { class: "grid-wrap" },
    el("table", { class: "grid" },
      el("thead", {}, el("tr", {},
        ...ALBUM_COLUMNS.map((f) => el("th", { scope: "col", class: `col-${f}` }, FIELDS[f].label)),
        el("th", { scope: "col" }, "Tracks"))),
      body));
  const filterNode = filterBar(() => {
    limit = SHOW_STEP;
    show();
  });
  show();
  return shell("albums", o,
    scanCard(o),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Albums ", count)),
      albums.length
        ? [filterNode,
           readOnly ? el("p", { class: "warn" }, "A scan or write is running: editing is " +
                                                 "possible again when it is done.") : null,
           legend(), table, el("div", { class: "actions" }, more)]
        : el("p", { class: "empty" }, "No albums yet. Scan the library.")));
}

/* ---- one album ----------------------------------------------------------- */

/* A track's value as it will be: the pending change, else the file's. */
function planned(t, f) {
  return f in t.pending ? t.pending[f] : t.tags[f];
}

function discOf(t) {
  return firstNumber(planned(t, "discnumber")) ?? 1;
}

/*
 * Numbering the album when track t gets x/y in field: every track's value
 * worked out again, X and Y as separate numbers. Disc: every track gets
 * Y discs; one without a disc number gets disc 1. Track: per disc, a track
 * keeps its number, one without gets the lowest free one (in file order),
 * Y is t's for t's disc and the larger of the count and the highest number
 * for the others. The edits for the tracks whose value changes.
 */
function renumber(tracks, t, field, x, y) {
  const edits = [];
  const set = (u, v) => {
    if (planned(u, field) !== v) edits.push({ track: u.id, field, value: v });
  };
  if (field === "discnumber") {
    for (const u of tracks) set(u, `${u === t ? x : discOf(u)}/${y}`);
    return edits;
  }
  const discs = new Map();
  for (const u of tracks) {
    const d = discOf(u);
    if (!discs.has(d)) discs.set(d, []);
    discs.get(d).push(u);
  }
  for (const list of discs.values()) {
    const numbers = new Map(list.map((u) => [u, u === t ? x : firstNumber(planned(u, field))]));
    const used = new Set([...numbers.values()].filter(Boolean));
    let next = 1;
    for (const u of list) {
      if (numbers.get(u)) continue;
      while (used.has(next)) next++;
      numbers.set(u, next);
      used.add(next);
    }
    const total = list.includes(t) ? y : Math.max(list.length, ...numbers.values());
    for (const u of list) set(u, `${numbers.get(u)}/${total}`);
  }
  return edits;
}

/* The Y the number popup starts with: discs in the album, or tracks on
 * t's disc (at least the highest number). */
function suggestedTotal(tracks, t, field) {
  if (field === "discnumber") return Math.max(1, ...tracks.map(discOf));
  const disc = tracks.filter((u) => discOf(u) === discOf(t));
  return Math.max(disc.length, ...disc.map((u) => firstNumber(planned(u, field)) ?? 0));
}

/* For each tag the tracks share, the value most of them have. */
function albumValues(tracks) {
  const common = {};
  for (const f of ALBUM_COLUMNS) {
    const counts = new Map();
    for (const t of tracks) {
      const k = JSON.stringify(planned(t, f) ?? null);
      counts.set(k, (counts.get(k) || 0) + 1);
    }
    let best = null;
    let most = 0;
    for (const [k, n] of counts) {
      if (n > most) {
        best = k;
        most = n;
      }
    }
    common[f] = counts.size > 1 ? best : null; /* null: all the same */
  }
  return common;
}

function trackOrder(a, b) {
  return discOf(a) - discOf(b) ||
         (firstNumber(planned(a, "tracknumber")) ?? 1e9) - (firstNumber(planned(b, "tracknumber")) ?? 1e9) ||
         a.path.localeCompare(b.path);
}

async function albumPage(id) {
  const [o, list] = await Promise.all([api("GET", "/api/music"),
                                       api("GET", `/api/music/album?track=${id}`)]);
  musicRoot = o.root || "";
  const tracks = [...list].sort(trackOrder);
  const readOnly = Boolean(o.busy);
  const common = albumValues(tracks);
  const name = planned(tracks[0], "album");
  const artist = planned(tracks[0], "albumartist");

  /* After a change the page is drawn again, back on the same control. */
  const redraw = async (key) => {
    await refresh();
    refocus(app, key);
  };
  const saveEdits = async (edits, key) => {
    if (edits.length) await queue({ edits });
    await redraw(key);
  };

  const rows = tracks.map((t) => el("tr", { class: Object.keys(t.pending).length ? "pending" : "" },
    ...TRACK_COLUMNS.map((f) => {
      const value = planned(t, f);
      const key = `${t.id}:${f}`;
      const marks = [f in t.pending ? "edited" : "",
                     common[f] != null && JSON.stringify(value ?? null) !== common[f] ? "differs" : "",
                     t.missing.includes(f) || t.invalid.includes(f) ? "bad" : ""].filter(Boolean);
      return editCell({
        field: f, value, mixed: false, marks, key, disabled: readOnly,
        save: (v) => saveEdits(sameValue(v, t.tags[f]) || !sameValue(v, value)
                                 ? [{ track: t.id, field: f, value: v }] : [], key),
        number: FIELDS[f].kind === "number" ? {
          x: firstNumber(value),
          y: suggestedTotal(tracks, t, f),
          save: (x, y) => saveEdits(renumber(tracks, t, f, x, y), key),
        } : null,
      });
    }),
    el("td", { class: "path muted" }, relative(t.path)),
    el("td", { class: "muted" }, t.ext),
    el("td", { class: "num muted" }, showSize(t.size)),
    el("td", { class: t.pictures.length ? "num muted" : "num missing-art" },
       String(t.pictures.length)),
    el("td", { class: "muted nowrap" }, showTime(t.scanned))));

  const refreshForm = serviceForm("/api/music/scan", { track: id },
    `Reads the ${plural(tracks.length, "file", "files")} of this album again. It changes no file.`,
    "Read again", "Scan started");
  return shell("albums", o,
    el("div", { class: "actions" },
      navButton("All albums", "#/albums"),
      readOnly || !o.available ? null
        : el("button", { class: "btn", type: "button", onclick: () => refreshForm.open() },
             "Read files again…")),
    refreshForm,
    el("header", { class: "section album-head" },
      el("h2", {}, name ?? "No album name"),
      el("p", {}, artist ?? "No album artist"),
      el("p", { class: "muted" }, plural(tracks.length, "track", "tracks"))),
    readOnly ? el("p", { class: "warn" }, "A scan or write is running: editing is possible " +
                                          "again when it is done.") : null,
    legend(),
    el("div", { class: "grid-wrap" },
      el("table", { class: "grid tracks" },
        el("thead", {}, el("tr", {},
          ...TRACK_COLUMNS.map((f) => el("th", { scope: "col", class: `col-${f}` }, FIELDS[f].label)),
          el("th", { scope: "col" }, "File"),
          el("th", { scope: "col" }, "Type"),
          el("th", { scope: "col" }, "Size"),
          el("th", { scope: "col" }, "Art"),
          el("th", { scope: "col" }, "Scanned"))),
        el("tbody", {}, rows))));
}

/* ---- changes ------------------------------------------------------------- */

/* One pending change as a table row: track, tag, now, new. */
function pendingRow(c) {
  const now = fromStored(c.field, c.now);
  const next = fromStored(c.field, c.value);
  return el("tr", {},
    el("td", {}, c.title ?? relative(c.path),
       c.title ? el("div", { class: "path muted small" }, relative(c.path)) : null),
    el("td", {}, FIELDS[c.field] ? FIELDS[c.field].label : c.field),
    el("td", { class: "old" }, showValue(c.field, now)),
    el("td", { class: "edited-text" }, next === "" ? "(removed)" : showValue(c.field, next)));
}

/* The pending changes of one batch, with Discard. */
function batchCard(batch, changes) {
  const discard = el("button", { class: "btn danger", type: "button", onclick: async () => {
    if (!sure(`Discard the ${plural(changes.length, "change", "changes")} of batch ${batch}?`)) return;
    discard.disabled = true;
    try {
      const r = await api("POST", "/api/music/discard", { batch });
      setStatus(`Discarded ${plural(r.discarded, "change", "changes")}`);
      refresh();
    } catch (err) {
      handleError(err);
      discard.disabled = false;
    }
  } }, "Discard");
  return el("section", { class: "card stack" },
    el("header", { class: "batch-head" },
      el("h3", {}, `Batch ${batch} `, el("span", { class: "count" }, changes.length)),
      discard),
    el("div", { class: "grid-wrap" },
      el("table", { class: "grid" },
        el("thead", {}, el("tr", {},
          el("th", { scope: "col" }, "Track"), el("th", { scope: "col" }, "Tag"),
          el("th", { scope: "col" }, "Now"), el("th", { scope: "col" }, "New"))),
        el("tbody", {}, changes.map(pendingRow)))));
}

async function changesPage() {
  const [o, ch] = await Promise.all([api("GET", "/api/music"), api("GET", "/api/music/changes")]);
  musicRoot = o.root || "";
  const batches = new Map();
  for (const c of ch.pending) {
    if (!batches.has(c.batch)) batches.set(c.batch, []);
    batches.get(c.batch).push(c);
  }
  const tracks = new Set(ch.pending.map((c) => c.track)).size;
  const write = serviceForm("/api/music/write", {},
    `Writes ${plural(ch.count, "change", "changes")} into the files, track by track. Each ` +
    "file is checked first (it must still be as scanned, and every tag valid), then written " +
    "and read back; each result is listed in Info.",
    "Write changes", "Writing started");
  const text = el("p", {}, busyText(o));
  const panel = el("section", { class: "card stack" },
    el("h2", {}, "Pending ", el("span", { class: "count" }, ch.count)),
    o.busy ? text : null,
    !o.available ? el("p", { class: "warn" },
                      "The music folder is not available (is the drive mounted?): nothing can " +
                      "be written now.") : null,
    ch.count === 0
      ? el("p", { class: "muted" }, "Nothing is queued. Edit albums to queue changes.")
      : el("p", { class: "muted" },
           `${plural(ch.count, "change", "changes")} to ${plural(tracks, "track", "tracks")}` +
           (ch.pending.length < ch.count ? ` (the first ${ch.pending.length} are listed)` : "") + "."),
    ch.count === 0 || o.busy || !o.available ? null
      : el("div", { class: "actions" },
          el("button", { class: "btn go", type: "button", onclick: () => write.open() },
             "Write changes…")),
    write);
  watchBusy(o, panel, text);
  return shell("changes", o, panel,
    [...batches].map(([batch, changes]) => batchCard(batch, changes)));
}

/* ---- info ---------------------------------------------------------------- */

const STATE_TEXT = { done: "Done", warning: "Done, with a warning", failed: "Failed" };

/* One written (or failed) change. Rose failed, peach warning. */
function historyItem(c) {
  const tone = c.state === "failed" ? "late" : c.state === "warning" ? "today" : "";
  const value = fromStored(c.field, c.value);
  return el("li", { class: "card stack history" },
    el("header", {},
      el("span", { class: tone ? `due ${tone}` : "muted" }, STATE_TEXT[c.state] || c.state),
      el("span", { class: "muted" }, showTime(c.finished))),
    el("strong", { class: "path" }, relative(c.path)),
    el("p", {},
      el("span", { class: "muted" }, `${FIELDS[c.field] ? FIELDS[c.field].label : c.field}: `),
      value === "" ? "(removed)" : showValue(c.field, value)),
    c.note ? el("p", { class: "note" }, c.note) : null);
}

const RULES = [
  "Every track needs: title, album, artist, album artist, track and disc number, date, " +
    "at least one genre and composer, and compilation.",
  "Text: UTF-8 without control characters, at most 500 bytes per value.",
  "Track and disc number: X/Y, two positive whole numbers, X at most Y (3/12). A track with " +
    "an invalid track number is not written; a missing or invalid disc number is written as 1/1.",
  "Date: the year, a positive whole number. BPM: a positive whole number.",
  "Genres: lowercase a-z, 0-9 and -, as many as needed, in order.",
  "Composers: as many as needed, in order.",
  "Compilation: yes or no (1 or 0).",
  "Titlesort, albumsort, artistsort, albumartistsort and composersort are written with " +
    "their tag (composersort: the composers joined by \"; \"), cut to 500 bytes if longer.",
];

async function infoPage() {
  const [o, ch] = await Promise.all([api("GET", "/api/music"), api("GET", "/api/music/changes")]);
  musicRoot = o.root || "";
  const s = o.stats;
  return shell("info", o,
    el("section", { class: "card stack" },
      el("h2", {}, "Library"),
      el("p", { class: "path muted" }, o.root),
      el("p", {}, `${plural(s.tracks, "track", "tracks")} in ${plural(s.albums, "album", "albums")}, ` +
                  `${showSize(s.size)}.`),
      s.extensions.length
        ? el("div", { class: "grid-wrap" },
            el("table", { class: "grid" },
              el("thead", {}, el("tr", {},
                el("th", { scope: "col" }, "Type"), el("th", { scope: "col", class: "num" }, "Tracks"),
                el("th", { scope: "col", class: "num" }, "Size"))),
              el("tbody", {}, s.extensions.map((x) => el("tr", {},
                el("td", {}, x.ext), el("td", { class: "num" }, String(x.tracks)),
                el("td", { class: "num" }, showSize(x.size)))))))
        : null,
      el("p", {}, scanSummary(o.scan)),
      el("p", { class: "muted" }, othersText(o.scan))),
    el("section", { class: "card stack" },
      el("h2", {}, "Rules"),
      el("ul", { class: "rules" }, RULES.map((r) => el("li", {}, r))),
      legend()),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Written changes ", el("span", { class: "count" }, ch.history.length))),
      ch.history.length
        ? el("ul", { class: "list cols" }, ch.history.map(historyItem))
        : el("p", { class: "empty" }, "Nothing written yet.")));
}

/* ---- routing ------------------------------------------------------------- */

/* ["album", "3"] -> that album, etc. Anything else goes to the albums. */
function route(parts) {
  const [section, rawId] = parts;
  const id = Number(rawId);
  if (section === "album" && Number.isInteger(id) && id > 0) return albumPage(id);
  if (section === "albums" && rawId === undefined) return albumsPage();
  if (section === "changes" && rawId === undefined) return changesPage();
  if (section === "info" && rawId === undefined) return infoPage();
  go("#/albums");
  return null;
}

startApp(route);
