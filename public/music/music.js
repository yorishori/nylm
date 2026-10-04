"use strict";

/*
 * Music app (/music/). Sections by URL hash:
 *   #/albums       every album (tracks sharing ALBUM and ALBUMARTIST) in an
 *                  editable table with filters, and the library scan
 *   #/album/ID     the tracks of the album of track ID, every tag editable
 *   #/changes      the queued changes by album and track: search them,
 *                  discard them, write them
 *   #/duplicates   possible duplicate tracks, albums and names: copy a
 *                  path, open an album, merge spellings of a name
 *   #/files        where the move service puts each file (its plan, its
 *                  problems, what it did), and starting it
 *   #/qobuz        connect to Qobuz, download albums into the library
 *   #/info         library counts, genres and years charts, the rules,
 *                  the written changes
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
/* Tags each track has its own: the album line leaves them blank. */
const PER_TRACK = ["discnumber", "tracknumber", "title", "bpm", "isrc", "musicbrainz_trackid"];

const MAX_BYTES = 500;
const MAX_VALUES = 64;
const MAX_SUGGESTIONS = 50;
const SHOW_STEP = 200; /* albums shown at a time */
const POLL_MS = 3000;
const POPUP_ROOM = 320; /* pixels an editing popup may grow to */
const GENRE_SLICES = 12;  /* genres in the donut; the rest are "other" */
const YEAR_BAR = 6;       /* pixels per year in the years chart */
const GENRE_RE = /^[a-z0-9-]+$/;
const WHOLE_RE = /^(?!0+$)\d{1,4}$/; /* a positive whole number, at most 9999 */
const encoder = new TextEncoder();
const COVER_SIDE = 1200;            /* pixels: the longer side of a new cover, at most */
const COVER_BYTES = 700 * 1024;     /* a new cover's JPEG, at most (the server's limit) */
const COVER_FIELD = "picture";      /* the change that sets the cover */

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

/* A tag's name, or "Cover" for a new cover. */
function fieldLabel(field) {
  if (field === COVER_FIELD) return "Cover";
  return FIELDS[field] ? FIELDS[field].label : field;
}

/* ---- pictures ------------------------------------------------------------- */

function artUrl(hash, size) {
  return `/api/music/art?hash=${encodeURIComponent(hash)}&size=${size}`;
}

/* A stored picture's thumbnail; if it can not be shown (no longer stored),
 * it is swapped for a muted note. */
function thumbImage(hash, alt, className) {
  const img = el("img", { src: artUrl(hash, "thumb"), alt, class: className, loading: "lazy",
                          decoding: "async" });
  img.addEventListener("error", () => {
    img.replaceWith(el("span", { class: `${className} no-art`, title: "Not stored any more" },
                       "gone"));
  });
  return img;
}

/* A list of pictures as small thumbnails, or "none". */
function thumbList(hashes, alt) {
  if (!hashes || !hashes.length) return el("span", { class: "muted" }, "none");
  return el("span", { class: "thumbs" }, hashes.map((h) => thumbImage(h, alt, "thumb small")));
}

/* "600 × 600 · JPEG · 2.4 KB" */
function pictureFacts(p) {
  const type = p.mime ? p.mime.replace("image/", "").toUpperCase() : "unknown type";
  const size = p.width ? `${p.width} × ${p.height} · ` : "";
  return `${size}${type} · ${showSize(p.size)}`;
}

/*
 * A new cover from a picture file: scaled to at most COVER_SIDE pixels and
 * made a JPEG of at most COVER_BYTES (scaledJpeg()). Resolves to
 * {canvas, blob}.
 */
async function makeCover(file) {
  const bitmap = await loadBitmap(file);
  try {
    const made = await scaledJpeg(bitmap, COVER_SIDE, COVER_BYTES);
    made.canvas.classList.add("cover-preview");
    return made;
  } finally {
    bitmap.close();
  }
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
  const tabs = [["albums", "Albums"], ["changes", changesLabel(overview.pending)],
                ["duplicates", "Duplicates"], ["files", "Files"], ["qobuz", "Qobuz"],
                ["info", "Info"]];
  return el("section", { class: "page" },
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
  if (o.busy === "move") return "Moving files…";
  if (o.busy === "qobuz") return "Saving downloaded files…";
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
                  : o.busy === "move" ? "Moving finished: see the results below"
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
    /* Positioned with the CSS object model: not an inline style attribute.
     * Below the anchor, or above it (growing upwards) when it would not fit
     * below and there is more room above. */
    const r = anchor.getBoundingClientRect();
    const below = window.innerHeight - r.bottom;
    /* Its suggestions come later: plan for at least POPUP_ROOM pixels. */
    const need = Math.max(pop.offsetHeight, POPUP_ROOM);
    if (r.bottom + 6 + need > window.innerHeight - 8 && r.top > below) {
      pop.style.bottom = `${Math.round(window.innerHeight - r.top + 6)}px`;
    } else {
      pop.style.top = `${Math.round(r.bottom + 6)}px`;
    }
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
                  invalid: false, missing: false, artists: false, noArt: false,
                  mixedArt: false, mixedGenres: false, invalidGenres: false,
                  none: { album: false, albumartist: false, date: false, composer: false,
                          genre: false } };
/* The fields with a text filter and a "no value" switch. */
const FILTER_FIELDS = ["album", "albumartist", "date", "composer", "genre"];

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
  for (const f of FILTER_FIELDS) {
    const v = filters[f].trim().toLowerCase();
    if (v && !albumText(a, f).includes(v)) return false;
    if (filters.none[f] && !a.missing.includes(f)) return false;
  }
  return !(filters.invalid && !a.invalid.length) && !(filters.missing && !a.missing.length) &&
         !(filters.artists && !a.several_artists) && !(filters.noArt && !a.no_art) &&
         !(filters.mixedArt && !a.mixed_art) &&
         !(filters.mixedGenres && !a.mixed.includes("genre")) &&
         !(filters.invalidGenres && !a.invalid.includes("genre"));
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
  const coverMarks = [a.changed.includes(COVER_FIELD) ? "edited" : "",
                      a.mixed_art ? "differs" : ""].filter(Boolean);
  tr.append(
    el("td", { class: ["col-cover", ...coverMarks].join(" ") },
      a.cover ? thumbImage(a.cover, "", "thumb")
              : el("span", { class: "thumb no-art", title: "No picture" }, "none")),
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
  const toggle = (set, key, label) => switchButton({ label, on: set[key], showLabel: true,
                                                     onToggle: (on) => {
                                                       set[key] = on;
                                                       show();
                                                       return true;
                                                     } });
  const LABELS = { album: "Album", albumartist: "Album artist", date: "Date",
                   composer: "Composer", genre: "Genre" };
  return el("div", { class: "filters stack" },
    text("q", "Search every column"),
    el("div", { class: "filter-fields" },
      FILTER_FIELDS.map((f) => el("div", { class: "filter-field" },
        text(f, LABELS[f]),
        toggle(filters.none, f, `No ${LABELS[f].toLowerCase()}`)))),
    el("div", { class: "filter-switches" },
      toggle(filters, "invalid", "Invalid tags"),
      toggle(filters, "missing", "Missing tags"),
      toggle(filters, "invalidGenres", "Invalid genres"),
      toggle(filters, "mixedGenres", "Genres differ between tracks"),
      toggle(filters, "artists", "Several artists, not a compilation"),
      toggle(filters, "noArt", "Tracks without art"),
      toggle(filters, "mixedArt", "Art differs between tracks")));
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
    return el("section", { class: "page" },
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
        el("th", { scope: "col", class: "col-cover" }, "Cover"),
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

/* A track's pictures as they will be: the new cover alone, else the file's. */
function plannedPictures(t) {
  return COVER_FIELD in t.pending ? [t.pending[COVER_FIELD]] : t.pictures.map((p) => p.hash);
}

/*
 * The album's pictures: each stored picture once, with how many tracks have
 * it, and the new cover (not written yet) first if there is one.
 */
function albumPictures(tracks) {
  const found = new Map();
  for (const t of tracks) {
    for (const p of t.pictures) {
      if (!found.has(p.hash)) found.set(p.hash, { ...p, tracks: new Set() });
      found.get(p.hash).tracks.add(t.id);
    }
  }
  const pending = tracks.filter((t) => COVER_FIELD in t.pending);
  return {
    next: pending.length ? { hash: pending[0].pending[COVER_FIELD], tracks: pending.length } : null,
    stored: [...found.values()],
  };
}

/* One picture of the album: thumbnail, what it is, how many tracks have it. */
function pictureCard(p, total, isNew) {
  const on = isNew ? p.tracks : p.tracks.size;
  return el("li", { class: isNew ? "card stack picture new" : "card stack picture" },
    p.mime === null && !isNew
      ? el("span", { class: "picture-none muted" }, "A type nylm does not show")
      : thumbImage(p.hash, isNew ? "New cover" : p.type || "Picture", "picture-image"),
    el("strong", {}, isNew ? "New cover" : p.type || "Picture"),
    isNew ? el("p", { class: "edited-text small" }, "Not written yet: it replaces every picture.")
          : el("p", { class: "muted small" }, pictureFacts(p)),
    !isNew && p.description ? el("p", { class: "small note" }, p.description) : null,
    el("p", { class: on === total ? "small" : "small differs-text" },
       on === total ? `On every track` : `On ${on} of ${plural(total, "track", "tracks")}`),
    isNew || p.mime === null ? null
      : el("div", { class: "actions" },
          el("a", { class: "btn", href: artUrl(p.hash, "full"), target: "_blank",
                    rel: "noopener" }, "Full size")));
}

/*
 * The album's pictures, and Set cover: a picture file is made a JPEG here
 * (see makeCover), shown, and queued for every track of album id.
 */
function picturesSection(id, tracks, readOnly) {
  const { next, stored } = albumPictures(tracks);
  const list = el("ul", { class: "pictures" },
    next ? pictureCard(next, tracks.length, true) : null,
    stored.map((p) => pictureCard(p, tracks.length, false)));
  const file = el("input", { type: "file", accept: "image/*", hidden: true });
  const preview = el("div", { class: "card stack", hidden: true });
  const choose = el("button", { class: "btn", type: "button", onclick: () => file.click() },
                    "Set cover…");
  file.addEventListener("change", async () => {
    const picked = file.files[0];
    file.value = "";
    if (!picked) return;
    choose.disabled = true;
    try {
      const { canvas, blob } = await makeCover(picked);
      const error = el("p", { class: "form-error", role: "alert" });
      const use = el("button", { class: "btn go", type: "button" }, "Use as cover");
      use.addEventListener("click", async () => {
        use.disabled = true;
        error.textContent = "";
        try {
          const r = await api("POST", "/api/music/cover", { album: id, image: await base64Of(blob) });
          setStatus(r.queued ? `Cover queued for ${plural(r.queued, "track", "tracks")}`
                             : "Every track has this cover already");
          refresh();
        } catch (err) {
          if (!(err instanceof ApiError && err.status === 401 && handleError(err))) {
            error.textContent = err.message;
          }
          use.disabled = false;
        }
      });
      preview.replaceChildren(
        el("h3", {}, "New cover"),
        canvas,
        el("p", { class: "muted small" },
           `${canvas.width} × ${canvas.height} · JPEG · ${showSize(blob.size)}`),
        el("p", { class: "hint" }, `Queued for every track of the album; writing makes it ` +
                                   "each track's only picture."),
        error,
        el("div", { class: "actions" }, use,
          el("button", { class: "btn", type: "button",
                         onclick: () => { preview.hidden = true; choose.focus(); } }, "Cancel")));
      preview.hidden = false;
      use.focus();
    } catch (err) {
      setStatus(err.message, true);
    } finally {
      choose.disabled = false;
    }
  });
  return el("section", { class: "section" },
    el("header", {}, el("h2", {}, "Pictures ", el("span", { class: "count" }, stored.length))),
    readOnly ? null : el("div", { class: "actions" }, choose, file),
    preview,
    next || stored.length ? list : el("p", { class: "empty" }, "No track has a picture."));
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
  /* The pictures most tracks have (null: all the same), as for the tags. */
  const pictureCounts = new Map();
  for (const t of tracks) {
    const k = JSON.stringify(plannedPictures(t));
    pictureCounts.set(k, (pictureCounts.get(k) || 0) + 1);
  }
  const commonPictures = pictureCounts.size > 1
    ? [...pictureCounts].sort((x, y) => y[1] - x[1])[0][0] : null;
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

  /* The album line: each tag the tracks share, edited for all at once
   * ("mixed" where they differ). */
  const albumLine = el("tr", { class: "album-line" },
    ...TRACK_COLUMNS.map((f) => {
      if (f === "title") return el("th", { scope: "row", class: "col-title" }, "All tracks");
      if (PER_TRACK.includes(f)) return el("td", { class: `col-${f}` });
      const values = new Set(tracks.map((t) => JSON.stringify(planned(t, f) ?? null)));
      const mixed = values.size > 1;
      const key = `album:${f}`;
      return editCell({
        field: f, value: mixed ? null : planned(tracks[0], f), mixed, key, disabled: readOnly,
        marks: tracks.some((t) => f in t.pending) ? ["edited"] : [],
        save: async (v) => {
          await queue({ album: id, set: { [f]: v } });
          await redraw(key);
        },
      });
    }),
    el("td", { colspan: "5", class: "muted small" }, "Edits here change every track."));

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
    el("td", { class: ["num", plannedPictures(t).length ? "muted" : "missing-art",
                       COVER_FIELD in t.pending ? "edited-text" : "",
                       commonPictures !== null && JSON.stringify(plannedPictures(t)) !== commonPictures
                         ? "differs-text" : ""]
                      .filter(Boolean).join(" ") },
       String(plannedPictures(t).length)),
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
      el("p", { class: "muted" }, plural(tracks.length, "track", "tracks")),
      searchButtons(artist, name)),
    readOnly ? el("p", { class: "warn" }, "A scan or write is running: editing is possible " +
                                          "again when it is done.") : null,
    picturesSection(id, tracks, readOnly),
    legend(),
    el("div", { class: "grid-wrap" },
      el("table", { class: "grid tracks" },
        el("thead", {}, el("tr", {},
          ...TRACK_COLUMNS.map((f) => el("th", { scope: "col", class: `col-${f}` }, FIELDS[f].label)),
          el("th", { scope: "col" }, "File"),
          el("th", { scope: "col" }, "Type"),
          el("th", { scope: "col" }, "Size"),
          el("th", { scope: "col" }, "Pictures"),
          el("th", { scope: "col" }, "Scanned"))),
        el("tbody", {}, albumLine, rows))));
}

/* Buttons that search other sites for the album, each in a new tab. */
function searchButtons(artist, name) {
  const q = [artist, name].filter(Boolean).join(" ");
  if (!q) return null;
  const sites = [
    ["RateYourMusic", "https://rateyourmusic.com/search", { searchterm: q, searchtype: "l" }],
    ["Wikipedia", "https://en.wikipedia.org/w/index.php", { search: q }],
    ["MusicBrainz", "https://musicbrainz.org/search", { query: q, type: "release_group" }],
  ];
  return el("div", { class: "actions" },
    sites.map(([label, base, params]) => el("button", {
      class: "btn", type: "button", "aria-label": `Search ${label} for this album`,
      onclick: () => window.open(`${base}?${new URLSearchParams(params)}`, "_blank", "noopener"),
    }, label)));
}

/* ---- changes ------------------------------------------------------------- */

/* One pending change as a table row: tag, now, new. A cover shows as
 * pictures: the track's now, the new one. */
function pendingRow(c) {
  const cover = c.field === COVER_FIELD;
  const now = cover ? JSON.parse(c.now || "[]") : fromStored(c.field, c.now);
  const next = cover ? c.value : fromStored(c.field, c.value);
  return el("tr", {},
    el("td", {}, fieldLabel(c.field)),
    el("td", { class: cover ? "" : "old" }, cover ? thumbList(now, "Picture now") : showValue(c.field, now)),
    el("td", { class: "edited-text" },
       cover ? thumbList([next], "New cover") : next === "" ? "(removed)" : showValue(c.field, next)));
}

/* The text the pending search looks in: album, artist, track, file, tag,
 * now and new. */
function pendingText(c) {
  const show = (v) => (c.field === COVER_FIELD ? "" : showValue(c.field, fromStored(c.field, v)));
  return [c.album, c.albumartist, c.title, relative(c.path), fieldLabel(c.field), show(c.now),
          show(c.value)].filter(Boolean).join("\n").toLowerCase();
}

/* A button that discards body's pending changes (what names them). */
function discardButton(body, what, total, disabled) {
  const button = el("button", { class: "btn danger", type: "button", disabled,
                                onclick: async () => {
    if (!sure(`Discard ${what}: ${plural(total, "change", "changes")}?`)) return;
    button.disabled = true;
    try {
      const r = await api("POST", "/api/music/discard", body);
      setStatus(`Discarded ${plural(r.discarded, "change", "changes")}`);
      refresh();
    } catch (err) {
      handleError(err);
      button.disabled = false;
    }
  } }, `Discard ${what}`);
  return button;
}

/* One album's pending changes (shown: those the search finds), track by
 * track, each with Discard; the whole album's with Discard album. The
 * changes of tracks no longer in the library are one group. */
function albumChanges(changes, shown, readOnly) {
  const first = changes[0];
  const removed = first.track == null;
  const tracks = new Map();
  for (const c of shown) {
    if (!tracks.has(c.track)) tracks.set(c.track, []);
    tracks.get(c.track).push(c);
  }
  const perTrack = new Map();
  for (const c of changes) perTrack.set(c.track, (perTrack.get(c.track) || 0) + 1);
  return el("section", { class: "card stack" },
    el("header", { class: "batch-head" },
      removed
        ? el("h3", {}, "No longer in the library ", el("span", { class: "count" }, changes.length))
        : el("div", {},
            el("h3", {}, first.album ?? "No album name", " ",
               el("span", { class: "count" }, changes.length)),
            el("p", { class: "muted" }, first.albumartist ?? "No album artist")),
      discardButton(removed ? { removed: true } : { album: first.track },
                    removed ? "these" : "album", changes.length, readOnly)),
    removed ? el("p", { class: "muted small" },
                 "A scan removed these tracks; writing marks their changes failed.") : null,
    [...tracks].map(([track, list]) => el("div", { class: "track-changes stack" },
      removed ? null : el("header", { class: "batch-head" },
        el("div", {},
          el("strong", {}, list[0].title ?? relative(list[0].path)),
          el("div", { class: "path muted small" }, relative(list[0].path))),
        discardButton({ track }, "track", perTrack.get(track), readOnly)),
      el("div", { class: "grid-wrap" },
        el("table", { class: "grid" },
          el("thead", {}, el("tr", {},
            el("th", { scope: "col" }, "Tag"), el("th", { scope: "col" }, "Now"),
            el("th", { scope: "col" }, "New"))),
          el("tbody", {}, list.map(pendingRow)))))));
}

/* The pending search; kept while the page is drawn again. */
let pendingSearch = "";

async function changesPage() {
  const [o, ch] = await Promise.all([api("GET", "/api/music"), api("GET", "/api/music/changes")]);
  musicRoot = o.root || "";
  /* By album: the server sends them in album order, removed tracks last. */
  const albums = new Map();
  for (const c of ch.pending) {
    const key = c.track == null ? "removed" : JSON.stringify([c.album, c.albumartist]);
    if (!albums.has(key)) albums.set(key, []);
    albums.get(key).push(c);
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
           `${plural(ch.count, "change", "changes")} to ${plural(tracks, "track", "tracks")} ` +
           `in ${plural(albums.size, "group", "groups")}` +
           (ch.pending.length < ch.count ? ` (the first ${ch.pending.length} are listed)` : "") + "."),
    ch.count === 0 || o.busy ? null
      : el("div", { class: "actions" },
          o.available ? el("button", { class: "btn go", type: "button",
                                       onclick: () => write.open() }, "Write changes…") : null,
          discardButton({ all: true }, "all", ch.count, false)),
    write);
  watchBusy(o, panel, text);
  if (!ch.pending.length) return shell("changes", o, panel);

  const texts = new Map(ch.pending.map((c) => [c, pendingText(c)]));
  const list = el("div", { class: "stack" });
  const found = el("p", { class: "muted", "aria-live": "polite" });
  const show = () => {
    const q = pendingSearch.trim().toLowerCase();
    const cards = [];
    let n = 0;
    for (const changes of albums.values()) {
      const shown = q ? changes.filter((c) => texts.get(c).includes(q)) : changes;
      n += shown.length;
      if (shown.length) cards.push(albumChanges(changes, shown, Boolean(o.busy)));
    }
    list.replaceChildren(...cards);
    found.textContent = q ? `${plural(n, "change", "changes")} found.` : "";
    if (q && !n) list.append(el("p", { class: "empty" }, "No pending change matches."));
  };
  const search = el("input", { type: "search", value: pendingSearch, "aria-label": "Search the changes",
                               placeholder: "Search the changes" });
  search.addEventListener("input", () => {
    pendingSearch = search.value;
    show();
  });
  show();
  return shell("changes", o, panel, el("div", { class: "filters stack" }, search, found), list);
}

/* ---- files --------------------------------------------------------------- */

const MOVE_STATE = { done: "Moved", failed: "Not moved", kept: "Stayed" };

/* A table shown SHOW_STEP rows at a time, with Show more. */
function steppedTable(head, rows) {
  const body = el("tbody");
  const more = el("button", { class: "btn", type: "button" }, "Show more");
  let limit = 0;
  const show = () => {
    limit += SHOW_STEP;
    body.replaceChildren(...rows.slice(0, limit));
    more.hidden = rows.length <= limit;
  };
  more.addEventListener("click", show);
  show();
  return [el("div", { class: "grid-wrap" },
            el("table", { class: "grid" },
              el("thead", {}, el("tr", {}, head.map((h) => el("th", { scope: "col" }, h)))),
              body)),
          el("div", { class: "actions" }, more)];
}

/* One thing the move service did. Rose: not moved; peach: stayed. */
function moveItem(m) {
  const tone = m.state === "failed" ? "late" : m.state === "kept" ? "today" : "";
  return el("li", { class: "card stack history" },
    el("header", {},
      el("span", { class: tone ? `due ${tone}` : "muted" },
         `${MOVE_STATE[m.state] || m.state}${m.track == null ? " (not a track)" : ""}`),
      el("span", { class: "muted" }, showTime(m.finished))),
    el("strong", { class: "path" }, relative(m.from_path)),
    m.to_path ? el("p", { class: "path" }, el("span", { class: "muted" }, "→ "),
                   relative(m.to_path)) : null,
    m.note ? el("p", { class: "note" }, m.note) : null);
}

async function filesPage() {
  const [o, mv] = await Promise.all([api("GET", "/api/music"), api("GET", "/api/music/moves")]);
  musicRoot = o.root || "";
  const start = serviceForm("/api/music/move", {},
    `Moves ${plural(mv.count, "file", "files")} to where their tags put them. A file is ` +
    "never moved over another; each move is listed below.",
    "Move files", "Moving started");
  const text = el("p", {}, busyText(o));
  const canStart = mv.count > 0 && !mv.pending && !o.busy && o.available;
  const panel = el("section", { class: "card stack" },
    el("h2", {}, "Files"),
    el("p", {}, "Each track belongs at Album artist/Album/NN - Title.ext below the music " +
                "folder, or D-NN - Title.ext when the album has several discs (NN: the track " +
                "number, D: the disc). The names come from the files' tags: / \\ : * ? \" < > | " +
                "and control characters become _, and a name never starts with a dot."),
    el("p", { class: "muted" }, "When all the tracks of a folder go to one folder, its other " +
                                "files (covers, logs, ...) go with them; emptied folders are " +
                                "removed."),
    o.busy ? text : null,
    !o.available ? el("p", { class: "warn" }, "The music folder is not available (is the " +
                                              "drive mounted?): nothing can be moved now.") : null,
    mv.pending ? el("p", { class: "warn" },
                    `${plural(mv.pending, "change is", "changes are")} pending: write or ` +
                    "discard them first, the names come from the files' tags.") : null,
    el("p", {}, mv.count ? `${plural(mv.count, "track moves", "tracks move")}.`
                         : "Every track that can be named is where it belongs.",
       mv.problem_count ? ` ${plural(mv.problem_count, "track can not", "tracks can not")} ` +
                          "move (see below)." : ""),
    canStart ? el("div", { class: "actions" },
                 el("button", { class: "btn go", type: "button", onclick: () => start.open() },
                    "Move files…")) : null,
    start);
  watchBusy(o, panel, text);
  const listed = (n, list) => (list.length < n ? ` (the first ${list.length})` : "");
  return shell("files", o, panel,
    mv.moves.length
      ? el("section", { class: "section" },
          el("header", {}, el("h2", {}, "To move ", el("span", { class: "count" }, mv.count),
                              listed(mv.count, mv.moves))),
          steppedTable(["From", "To"], mv.moves.map((m) => el("tr", {},
            el("td", { class: "path" }, relative(m.from)),
            el("td", { class: "path edited-text" }, relative(m.to))))))
      : null,
    mv.problems.length
      ? el("section", { class: "section" },
          el("header", {}, el("h2", {}, "Can not move ",
                              el("span", { class: "count" }, mv.problem_count),
                              listed(mv.problem_count, mv.problems))),
          steppedTable(["File", "Why"], mv.problems.map((p) => el("tr", {},
            el("td", { class: "path" }, relative(p.path)),
            el("td", { class: "differs-text" }, p.problem)))))
      : null,
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Moved ", el("span", { class: "count" }, mv.history.length))),
      mv.history.length
        ? el("ul", { class: "list cols" }, mv.history.map(moveItem))
        : el("p", { class: "empty" }, "Nothing moved yet.")));
}

/* ---- qobuz --------------------------------------------------------------- */

const DOWNLOAD_STATE = { queued: "Waiting", running: "Downloading", done: "Done",
                         warning: "Done, with a problem", failed: "Failed" };

/* A form that starts the Qobuz service with the password and what
 * inputs() gives (it throws invalid() if something is missing). */
function qobuzForm(intro, inputs, submitLabel, started, ...fields) {
  const password = el("input", { type: "password", name: "password", required: true,
                                 autocomplete: "current-password" });
  return form({ class: "raised" }, async () => {
    const extra = inputs();
    try {
      await api("POST", "/api/music/qobuz/start", { ...extra, password: password.value });
    } finally {
      password.value = "";
    }
    setStatus(started);
    refresh();
  },
    el("p", {}, intro),
    ...fields,
    field("Password", password, "Starting the Qobuz service needs your password again."),
    el("div", { class: "actions" }, el("button", { class: "btn go", type: "submit" }, submitLabel)));
}

/* One download: the album, how it went. Rose failed, peach a problem. */
function downloadItem(d) {
  const tone = d.state === "failed" ? "late" : d.state === "warning" ? "today" : "";
  return el("li", { class: "card stack history" },
    el("header", {},
      el("span", { class: tone ? `due ${tone}` : "muted" }, DOWNLOAD_STATE[d.state] || d.state),
      el("span", { class: "muted" }, showTime(d.finished || d.started || d.requested))),
    el("strong", {}, d.title || `Album ${d.album_id}`),
    d.artist ? el("p", { class: "muted" }, d.artist) : null,
    d.tracks ? el("p", {}, `${d.saved} of ${plural(d.tracks, "track", "tracks")} saved`) : null,
    d.note ? el("p", { class: "note" }, d.note) : null);
}

async function qobuzPage() {
  const [o, qb] = await Promise.all([api("GET", "/api/music"), api("GET", "/api/music/qobuz")]);
  musicRoot = o.root || "";
  const status = el("p", {}, qb.running ? "The Qobuz service is running…" : "");
  /* While it runs, check back; when it ends, show the page again. */
  if (qb.running) {
    const poll = async () => {
      if (!status.isConnected) return;
      try {
        const now = await api("GET", "/api/music/qobuz");
        if (!status.isConnected) return;
        if (now.running) setTimeout(poll, POLL_MS);
        else {
          setStatus("Qobuz finished");
          refresh();
        }
      } catch (err) {
        handleError(err);
      }
    };
    setTimeout(poll, POLL_MS);
  }
  const ready = o.available && !qb.running;

  const pasted = el("input", { type: "text", name: "login", required: true, autocomplete: "off",
                               spellcheck: "false", maxlength: 2048 });
  const connect = qb.login_url
    ? qobuzForm("Log in at Qobuz. Qobuz then opens an address on localhost that does not load: " +
                "copy that whole address from the address bar and paste it here.",
                () => ({ login: pasted.value.trim() }), "Connect", "Connecting to Qobuz",
                el("div", { class: "actions" },
                  el("button", { class: "btn", type: "button",
                                 onclick: () => window.open(qb.login_url, "_blank", "noopener") },
                     "Open the Qobuz login")),
                field("Address Qobuz opened", pasted))
    : qobuzForm("First nylm reads the Qobuz web player's app id, which the login link needs.",
                () => ({}), "Get the login link", "Getting the login link");

  const links = el("textarea", { name: "urls", rows: 4, required: true, spellcheck: "false",
                                 placeholder: "https://www.qobuz.com/us-en/album/name/0123456789" });
  const download = qobuzForm(
    "Album links, one per line (at most 50). Each album is downloaded in the best FLAC Qobuz " +
    "has, tagged from Qobuz (no genre: add your own), saved where the naming rule puts it " +
    "(never over a file) and scanned.",
    () => {
      const urls = links.value.split("\n").map((l) => l.trim()).filter(Boolean);
      if (!urls.length) throw invalid("Paste at least one album link.");
      return { urls };
    }, "Download", "Downloading started", field("Album links", links));

  const account = el("section", { class: "card stack" },
    el("h2", {}, "Qobuz"),
    el("p", {}, qb.connected ? `Connected${qb.label ? ` (${qb.label})` : ""}.`
                             : "Not connected."),
    qb.login_pending ? el("p", { class: "muted" }, "A login waits for the Qobuz service.") : null,
    qb.error ? el("p", { class: "differs-text" }, qb.error) : null,
    !o.available ? el("p", { class: "warn" }, "The music folder is not available (is the " +
                                              "drive mounted?).") : null,
    status);
  const connectSection = el("section", { class: "section" },
    el("header", {}, el("h2", {}, qb.connected ? "Connect again" : "Connect")), connect);
  return shell("qobuz", o, account,
    ready && qb.connected ? el("section", { class: "section" },
                              el("header", {}, el("h2", {}, "Download")), download) : null,
    ready ? connectSection : null,
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Downloads ", el("span", { class: "count" }, qb.downloads.length))),
      qb.downloads.length
        ? el("ul", { class: "list cols" }, qb.downloads.map(downloadItem))
        : el("p", { class: "empty" }, "Nothing downloaded yet.")));
}

/* ---- duplicates ---------------------------------------------------------- */

/* The lists of GET /api/music/duplicates: group, list, label, what a row is
 * (a track, an album, or a name of that field). */
const DUPLICATE_LISTS = [
  ["tracks", "trackid", "Tracks: same MusicBrainz track ID", "track"],
  ["tracks", "isrc", "Tracks: same ISRC", "track"],
  ["tracks", "title", "Tracks: same artist and title", "track"],
  ["albums", "albumid", "Albums: same MusicBrainz album ID", "album"],
  ["albums", "barcode", "Albums: same barcode", "album"],
  ["albums", "name", "Albums: same album and album artist", "album"],
  ["albums", "title", "Albums: same album, other album artist", "album"],
  ["names", "artist", "Artists", "artist"],
  ["names", "albumartist", "Album artists", "albumartist"],
  ["names", "composer", "Composers", "composer"],
  ["names", "genre", "Genres", "genre"],
];

/* The list shown; kept while the page is drawn again. */
let duplicateList = null;

/* Copies text to the clipboard (what: what it is, for the status line).
 * Over plain HTTP the browser has no clipboard API: a selected text area
 * is copied instead. If that fails too, the status line shows the text. */
async function copyText(text, what) {
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text);
    } else {
      const back = document.activeElement;
      const area = el("textarea", { class: "offscreen", readonly: true, "aria-hidden": "true" });
      area.value = text;
      document.body.append(area);
      area.select();
      const ok = document.execCommand("copy");
      area.remove();
      if (back) back.focus();
      if (!ok) throw new Error("copy refused");
    }
    setStatus(`Copied the ${what}: ${text}`);
  } catch {
    setStatus(`Could not copy; the ${what} is: ${text}`, true);
  }
}

/* A list's rows as groups (rows that share a key are together). When the
 * list was cut its last group may be too: it is left out. */
function duplicateGroups(section) {
  const groups = [];
  for (const r of section.rows) {
    const last = groups[groups.length - 1];
    if (last && last[0].key === r.key) last.push(r);
    else groups.push([r]);
  }
  if (section.more && groups.length > 1) groups.pop();
  return groups;
}

/* One possible duplicate (a track or an album): what it is, where it is,
 * Copy and Open album. */
function duplicateItem(title, facts, path, what, id) {
  return el("div", { class: "dupe" },
    el("div", { class: "dupe-text" },
      el("strong", {}, title),
      el("p", { class: "muted" }, facts),
      el("p", { class: "path small" }, relative(path))),
    el("div", { class: "actions" },
      el("button", { class: "btn", type: "button", onclick: () => copyText(path, what) },
         `Copy ${what}`),
      navButton("Open album", `#/album/${id}`)));
}

function trackGroup(group) {
  return el("li", { class: "card stack" },
    group.map((t) => duplicateItem(t.title ?? "No title",
      `${t.artist ?? "No artist"} · ${t.album ?? "No album"} · ${t.albumartist ?? "No album artist"}`,
      t.path, "path", t.id)));
}

function albumGroup(group) {
  return el("li", { class: "card stack" },
    group.map((a) => duplicateItem(a.album ?? "No album name",
      `${a.albumartist ?? "No album artist"} · ${plural(a.tracks, "track", "tracks")}`,
      a.folder, "folder", a.id)));
}

/* The result of a merge for the status line. */
function mergeText(r) {
  const parts = [];
  if (r.queued) parts.push(`Queued: ${plural(r.queued, "change", "changes")} (write them from Changes)`);
  if (r.dropped) parts.push(`${plural(r.dropped, "track is", "tracks are")} back to the files' value`);
  if (r.skipped) {
    parts.push(`${plural(r.skipped, "track was", "tracks were")} left alone: another of its ` +
               "values breaks the rules; fix it in the album");
  }
  return parts.length ? parts.join(". ") : "Nothing to change";
}

/* Changes every other name of the group to into, on every track. */
function mergeButton(field, into, others, disabled) {
  const names = others.map((v) => `"${v.value}"`).join(", ");
  const tracks = others.reduce((n, v) => n + v.tracks, 0);
  const button = el("button", {
    class: "btn go", type: "button", disabled,
    "aria-label": `Change ${names} to "${into.value}"`,
    onclick: async () => {
      if (!sure(`Change ${names} to "${into.value}" on ${plural(tracks, "track", "tracks")}? ` +
                "The changes are queued; nothing is written until you write them.")) return;
      button.disabled = true;
      try {
        const r = await api("POST", "/api/music/merge",
                            { field, from: others.map((v) => v.value), to: into.value });
        valueCache.clear();
        setStatus(mergeText(r));
        refresh();
      } catch (err) {
        handleError(err);
        button.disabled = false;
      }
    },
  }, "Use this one");
  return button;
}

/* A group of spellings of one name, each with its tracks and Use this one
 * (unless the name breaks the rules, or a service runs). */
function nameGroup(field, group, busy) {
  return el("li", { class: "card stack" },
    group.map((v) => {
      const bad = problem(field, FIELDS[field].kind === "list" ? [v.value] : v.value);
      return el("div", { class: "dupe" },
        el("div", { class: "dupe-text" },
          el("strong", { class: "path" }, v.value),
          el("p", { class: bad ? "differs-text small" : "muted" },
             bad ? `Breaks the rules: ${bad}` : plural(v.tracks, "track", "tracks"))),
        el("div", { class: "actions" },
          mergeButton(field, v, group.filter((x) => x !== v), busy || Boolean(bad))));
    }));
}

async function duplicatesPage() {
  const [o, d] = await Promise.all([api("GET", "/api/music"), api("GET", "/api/music/duplicates")]);
  musicRoot = o.root || "";
  const lists = DUPLICATE_LISTS.map(([group, name, label, kind]) => {
    const section = d[group][name];
    return { key: `${group}/${name}`, label, kind, more: section.more,
             groups: duplicateGroups(section) };
  });
  if (!lists.some((l) => l.key === duplicateList)) {
    duplicateList = (lists.find((l) => l.groups.length) || lists[0]).key;
  }
  const body = el("div", { class: "stack" });
  const show = () => {
    const l = lists.find((x) => x.key === duplicateList);
    const card = (g) => (l.kind === "track" ? trackGroup(g)
                         : l.kind === "album" ? albumGroup(g)
                         : nameGroup(l.kind, g, Boolean(o.busy)));
    body.replaceChildren(...[
      l.more ? el("p", { class: "warn" },
                  `Only the first ${plural(l.groups.length, "group is", "groups are")} listed: ` +
                  "deal with these, then look again.") : null,
      l.groups.length ? el("ul", { class: "list cols" }, l.groups.map(card))
                      : el("p", { class: "empty" }, "None found."),
    ].filter(Boolean));
  };
  const picker = dropdown({
    label: "Duplicates to show",
    options: lists.map((l) => ({ value: l.key,
                                 label: `${l.label} (${l.groups.length}${l.more ? "+" : ""})` })),
    value: duplicateList,
    onchange: (v) => {
      duplicateList = v;
      show();
    },
  });
  show();
  return shell("duplicates", o,
    el("section", { class: "card stack" },
      el("h2", {}, "Duplicates"),
      el("p", {}, "Possible duplicates, by the tags with the pending changes. Names are the " +
                  "same when they differ only in case, accents, punctuation and spaces, a " +
                  "leading \"The\", or & for \"and\". An album here is the tracks of one album " +
                  "and album artist in one folder."),
      el("p", { class: "muted" }, "Nothing here removes a file: copy a track's path or an " +
                                  "album's folder and deal with the files yourself. Use this " +
                                  "one queues the change of the other spellings on every track, " +
                                  "like any edit."),
      o.busy ? el("p", { class: "warn" }, `${busyText(o)} Names can be merged when it is done.`)
             : null,
      field("Show", picker)),
    body);
}

/* ---- info ---------------------------------------------------------------- */

const STATE_TEXT = { done: "Done", warning: "Done, with a warning", failed: "Failed" };

/* One written (or failed) change. Rose failed, peach warning. */
function historyItem(c) {
  const tone = c.state === "failed" ? "late" : c.state === "warning" ? "today" : "";
  const cover = c.field === COVER_FIELD;
  const value = cover ? c.value : fromStored(c.field, c.value);
  return el("li", { class: "card stack history" },
    el("header", {},
      el("span", { class: tone ? `due ${tone}` : "muted" }, STATE_TEXT[c.state] || c.state),
      el("span", { class: "muted" }, showTime(c.finished))),
    el("strong", { class: "path" }, relative(c.path)),
    el("p", {},
      el("span", { class: "muted" }, `${fieldLabel(c.field)}: `),
      cover ? thumbList([value], "Cover") : value === "" ? "(removed)" : showValue(c.field, value)),
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
  "Cover: a picture chosen here is scaled to at most 1200 pixels and sent as a JPEG of at " +
    "most 700 KiB. It must decode completely when written; then it is each track's only " +
    "picture, as the front cover.",
];

/* An SVG element, like el(). */
function svg(tag, attrs, ...children) {
  const node = document.createElementNS("http://www.w3.org/2000/svg", tag);
  for (const [key, value] of Object.entries(attrs || {})) node.setAttribute(key, value);
  node.append(...children.flat().filter((c) => c != null)
                 .map((c) => (c instanceof Node ? c : document.createTextNode(String(c)))));
  return node;
}

function percent(n, total) {
  const p = (100 * n) / total;
  return p < 1 ? "<1%" : `${Math.round(p)}%`;
}

/* The genres as a donut: the largest GENRE_SLICES, the rest as "other";
 * a legend beside it, and every genre in a table on demand. */
function genreChart(genres) {
  if (!genres.length) return el("p", { class: "muted" }, "No genres yet.");
  const total = genres.reduce((n, g) => n + g.albums, 0);
  const slices = genres.slice(0, GENRE_SLICES).map((g, i) => ({ ...g, color: `c${i}` }));
  const rest = genres.slice(GENRE_SLICES).reduce((n, g) => n + g.albums, 0);
  if (rest) slices.push({ genre: `other (${genres.length - GENRE_SLICES})`, albums: rest, color: "other" });
  let start = 0;
  const rings = slices.map((s) => {
    const share = (100 * s.albums) / total;
    const ring = svg("circle", { class: `slice ${s.color}`, cx: 50, cy: 50, r: 38, pathLength: 100,
                                 "stroke-dasharray": `${share} ${100 - share}`,
                                 "stroke-dashoffset": String(-start) },
                     svg("title", {}, `${s.genre}: ${plural(s.albums, "album", "albums")}`));
    start += share;
    return ring;
  });
  const table = el("div", { class: "grid-wrap", hidden: true },
    el("table", { class: "grid" },
      el("thead", {}, el("tr", {},
        el("th", { scope: "col" }, "Genre"), el("th", { scope: "col", class: "num" }, "Albums"))),
      el("tbody", {}, genres.map((g) => el("tr", {},
        el("td", {}, g.genre), el("td", { class: "num" }, String(g.albums)))))));
  const toggle = el("button", { class: "btn", type: "button", onclick: () => {
    table.hidden = !table.hidden;
    toggle.textContent = table.hidden ? `Show all ${genres.length} genres` : "Hide the genres";
  } }, `Show all ${genres.length} genres`);
  return el("div", { class: "stack" },
    el("p", { class: "muted small" },
       "Albums by genre; an album with several genres counts for each."),
    el("div", { class: "donut-chart" },
      svg("svg", { viewBox: "0 0 100 100", class: "donut", role: "img",
                   "aria-label": "Albums by genre" },
        svg("g", { transform: "rotate(-90 50 50)" }, rings)),
      el("ul", { class: "chart-legend" }, slices.map((s) => el("li", {},
        el("span", { class: `swatch ${s.color}` }),
        el("span", { class: "legend-name" }, s.genre),
        el("span", { class: "muted nowrap" }, `${s.albums} · ${percent(s.albums, total)}`))))),
    el("div", { class: "actions" }, toggle),
    table);
}

/* Albums per year: a bar for every year from the first to the last (a
 * date counts by its first four digits), scrolling sideways. */
function yearChart(dates) {
  const years = new Map();
  let undated = 0;
  for (const d of dates) {
    const m = /^(\d{4})/.exec(d.date);
    if (m) years.set(Number(m[1]), (years.get(Number(m[1])) || 0) + d.albums);
    else undated += d.albums;
  }
  if (!years.size) return el("p", { class: "muted" }, "No years yet.");
  const first = Math.min(...years.keys());
  const last = Math.max(...years.keys());
  const most = Math.max(...years.values());
  const W = YEAR_BAR, H = 120, AXIS = 16;
  const bars = [], labels = [];
  for (let y = first; y <= last; y++) {
    const x = (y - first) * W;
    const n = years.get(y) || 0;
    if (n) {
      const h = Math.max(1, Math.round((H * n) / most));
      bars.push(svg("rect", { class: "bar", x: x + 1, y: H - h, width: W - 2, height: h },
                    svg("title", {}, `${y}: ${plural(n, "album", "albums")}`)));
    }
    if (y % 10 === 0) {
      labels.push(svg("line", { class: "tick", x1: x, x2: x, y1: H, y2: H + 4 }),
                  svg("text", { class: "tick-label", x, y: H + AXIS - 2 }, String(y)));
    }
  }
  const width = (last - first + 1) * W;
  return el("div", { class: "stack" },
    el("p", { class: "muted small" },
       `Albums by year, ${first} to ${last}; the most in one year: ${most}.` +
       (undated ? ` ${plural(undated, "album has", "albums have")} a date that is not a year.` : "")),
    el("div", { class: "chart-scroll" },
      svg("svg", { class: "years", width: width + 30, height: H + AXIS, role: "img",
                   viewBox: `0 0 ${width + 30} ${H + AXIS}`, "aria-label": "Albums by year" },
        svg("line", { class: "axis", x1: 0, x2: width, y1: H, y2: H }), bars, labels)));
}

async function infoPage() {
  const [o, ch, charts] = await Promise.all([api("GET", "/api/music"),
                                             api("GET", "/api/music/changes"),
                                             api("GET", "/api/music/charts")]);
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
    el("section", { class: "card stack" }, el("h2", {}, "Genres"), genreChart(charts.genres)),
    el("section", { class: "card stack" }, el("h2", {}, "Years"), yearChart(charts.dates)),
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
  if (section === "duplicates" && rawId === undefined) return duplicatesPage();
  if (section === "files" && rawId === undefined) return filesPage();
  if (section === "qobuz" && rawId === undefined) return qobuzPage();
  if (section === "info" && rawId === undefined) return infoPage();
  go("#/albums");
  return null;
}

startApp(route);
