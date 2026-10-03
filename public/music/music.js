"use strict";

/*
 * Music app (/music/). Sections by URL hash:
 *   #/albums      every album in an editable table, and the library scan
 *   #/album/ID    one album: its album-wide tags and each track's
 *   #/changes     the albums with queued changes (current and new values),
 *                 discard, write them; and the history
 *
 * The server never touches the files: it keeps a cache of their tags and a
 * queue of changes. Every edit here is queued as soon as a cell is left
 * (no save button); the write service puts the queue into the files, the
 * scan reads the files into the cache. Both are started from this page,
 * with the password again. While one runs, nothing can be queued.
 *
 * A cell shows a tag and is pressed to edit it: text in place, a list
 * (artist, album artist, genre, composer) in a popup of chips with
 * suggestions, compilation as yes / no.
 */

/* Every tag the page edits: label, kind of editor, whether it may be empty. */
const FIELDS = {
  album: { label: "Album", kind: "text", required: true },
  albumartist: { label: "Album artist", kind: "list", required: true },
  genre: { label: "Genre", kind: "list" },
  composer: { label: "Composer", kind: "list" },
  date: { label: "Date", kind: "text", placeholder: "YYYY or YYYY-MM-DD" },
  compilation: { label: "Compilation", kind: "bool" },
  discnumber: { label: "Disc", kind: "text", required: true, placeholder: "1/1" },
  tracknumber: { label: "No.", kind: "text", required: true, placeholder: "3 or 3/12" },
  title: { label: "Title", kind: "text", required: true },
  artist: { label: "Artist", kind: "list", required: true },
};
/* The album-wide tags: the albums table's columns. */
const ALBUM_COLUMNS = ["album", "albumartist", "genre", "composer", "date", "compilation"];
/* The track tags: the album page's columns. */
const TRACK_COLUMNS = ["discnumber", "tracknumber", "title", "artist", "composer"];

const MAX_VALUE = 500; /* bytes; the server checks the exact limit */
const MAX_SUGGESTIONS = 50;
const POLL_MS = 3000;
const GENRE_RE = /^[a-z-]+( [a-z-]+)*$/;

/* "3:05" or "1:02:03". */
function duration(seconds) {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = pad2(seconds % 60);
  return h ? `${h}:${pad2(m)}:${s}` : `${m}:${s}`;
}

/* A unix time as "Sat 3 Oct 14:05". */
function showTime(ts) {
  const d = new Date(ts * 1000);
  return `${showDate(isoDate(d.getFullYear(), d.getMonth() + 1, d.getDate()))} ` +
         `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
}

/* A tag value for people: compilation "1" is "yes", nothing is "(none)". */
function showValue(field, v) {
  if (v == null || v === "") return field === "compilation" ? "no" : "(none)";
  if (field === "compilation") return v === "1" ? "yes" : v;
  return v;
}

/* A list value split into its values ("A; B" -> ["A", "B"]). */
function splitList(v) {
  return v ? v.split("; ").filter(Boolean) : [];
}

/* ---- shell -------------------------------------------------------------- */

const tabLabels = {};

function changesLabel(pending) {
  return pending ? `Changes (${pending})` : "Changes";
}

function shell(active, overview, ...content) {
  const tabs = [["albums", "Albums"], ["changes", changesLabel(overview.pending)]];
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

/* ---- services ----------------------------------------------------------- */

function busyText(o) {
  if (o.busy === "scan") {
    const s = o.scan;
    return s && s.finished == null ? `Scanning… ${plural(s.files, "file", "files")} so far.`
                                   : "Scanning…";
  }
  if (o.busy === "write") return "Writing changes to the files…";
  return o.busy ? "Starting…" : "";
}

/*
 * While a service runs, checks back every few seconds: text shows the
 * progress; when it ends the page is shown again. node: stop when it is
 * no longer on the page.
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
        setStatus(o.busy === "write" ? "Writing finished: see the results in Changes"
                                     : "Scan finished");
        refresh();
      }
    } catch (err) {
      handleError(err);
    }
  };
  setTimeout(poll, POLL_MS);
}

/* A password form that starts a service (path), then shows the page again. */
function serviceForm(path, intro, submitLabel, started) {
  const password = el("input", { type: "password", name: "password", required: true,
                                 autocomplete: "current-password" });
  const node = form({ class: "raised", hidden: true }, async () => {
    try {
      await api("POST", path, { password: password.value });
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

function scanText(o) {
  const s = o.scan;
  switch (o.scan_state) {
    case "never":
      return "Never scanned.";
    case "running":
      return busyText(o);
    case "interrupted":
      return `The scan started ${showTime(s.started)} did not finish.`;
    case "failed":
      return `Last scan ${showTime(s.finished)}: some folders could not be read, so no ` +
             "missing files were removed. See the server log.";
    default:
      return `Last scan ${showTime(s.finished)}: ${plural(s.files, "file", "files")}` +
             (s.failed ? `, ${s.failed} could not be read (see the server log).` : ".");
  }
}

/* Counts, the last scan, and the scan button. */
function libraryCard(o) {
  const text = el("p", {}, o.busy && o.busy !== "scan" ? busyText(o) : scanText(o));
  const scan = serviceForm("/api/music/scan",
    "The scan reads new and changed files into nylm. It changes no file.",
    "Start scan", "Scan started");
  const card = el("section", { class: "card stack" },
    el("h2", {}, "Library"),
    el("p", {}, `${plural(o.albums, "album", "albums")}, ${plural(o.tracks, "track", "tracks")}`),
    o.available ? null
                : el("p", { class: "warn" },
                     "The music folder is not available (is the drive mounted?). The albums " +
                     "below are from the last scan; nothing can be scanned or written now."),
    text,
    o.busy || !o.available ? null
      : el("div", { class: "actions" },
          el("button", { class: "btn", type: "button", onclick: () => scan.open() },
             "Scan library…")),
    scan);
  watchBusy(o, card, text);
  return card;
}

/* ---- cells and their editors -------------------------------------------- */

/* The values in use for a list field, fetched once per page load. */
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

/* What is wrong with one value of a list field, or null. */
function listValueProblem(field, v) {
  if (v === "") return "Type a value first.";
  if (v.includes(";")) return "A value can not contain ';'.";
  if (field === "genre" && !GENRE_RE.test(v)) {
    return "A genre is lowercase a-z and -, words separated by single spaces.";
  }
  return null;
}

/*
 * A popup that sits under anchor (fixed, so a scrolling table does not cut
 * it off) and closes on Escape or a press outside. close(refocus) is
 * called once. Returns {pop, close}.
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
 * The chip editor for a list field. initial: its values; mixed: tracks
 * differ (nothing is changed unless values are chosen). onSave(value) gets
 * the "; "-joined string. Returns the popup's close().
 */
function listEditor(anchor, { field, initial, mixed, onSave, onDone }) {
  const spec = FIELDS[field];
  let chosen = [...initial];
  let touched = false;
  let active = -1;
  let shown = [];

  const { pop, close } = floatingPopup(anchor, spec.label, (refocus) => onDone(refocus));
  const chips = el("ul", { class: "chips", "aria-label": `${spec.label} values` });
  const input = el("input", { maxlength: MAX_VALUE, placeholder: "Add a value…",
                              "aria-label": `Add ${spec.label.toLowerCase()}`,
                              role: "combobox", "aria-autocomplete": "list",
                              "aria-expanded": "false" });
  const listId = uniqueId("suggest");
  const list = el("ul", { class: "listbox suggestions", role: "listbox", id: listId,
                          "aria-label": "Suggestions" });
  input.setAttribute("aria-controls", listId);
  const error = el("p", { class: "form-error", role: "alert" });
  let known = [];

  function renderChips() {
    chips.replaceChildren(...chosen.map((v, i) => el("li", { class: "chip value" },
      el("span", {}, v),
      el("button", { type: "button", class: "chip-x", "aria-label": `Remove ${v}`,
                     onclick: () => { chosen.splice(i, 1); touched = true; renderChips(); input.focus(); } },
         "×"))));
    if (!chosen.length) {
      chips.append(el("li", { class: "muted" }, mixed && !touched ? "Differs between tracks" : "No values"));
    }
  }
  function renderSuggestions() {
    const q = input.value.trim().toLowerCase();
    shown = known.filter((v) => !chosen.includes(v) && (!q || v.toLowerCase().includes(q)))
                 .slice(0, MAX_SUGGESTIONS);
    active = Math.min(active, shown.length - 1);
    list.replaceChildren(...shown.map((v, i) => el("li", {
      role: "option", id: `${listId}-${i}`, class: i === active ? "option active" : "option",
      "aria-selected": String(i === active),
      onpointerdown: (e) => e.preventDefault(), /* keep the focus in the input */
      onclick: () => add(v),
    }, v)));
    input.setAttribute("aria-expanded", String(shown.length > 0));
    if (active >= 0) input.setAttribute("aria-activedescendant", `${listId}-${active}`);
    else input.removeAttribute("aria-activedescendant");
  }
  function add(v) {
    const value = v.trim();
    const problem = listValueProblem(field, value);
    if (problem) {
      error.textContent = problem;
      return;
    }
    error.textContent = "";
    if (!chosen.includes(value)) chosen.push(value);
    touched = true;
    input.value = "";
    active = -1;
    renderChips();
    renderSuggestions();
    input.focus();
  }
  async function save() {
    if (!touched) {
      close(true);
      return;
    }
    if (!chosen.length && mixed) {
      close(true); /* nothing chosen for differing tracks: keep each */
      return;
    }
    if (!chosen.length && spec.required) {
      error.textContent = `${spec.label} needs at least one value.`;
      return;
    }
    const ok = await onSave(chosen.join("; "));
    if (ok) close(true);
  }

  input.addEventListener("input", () => { active = -1; error.textContent = ""; renderSuggestions(); });
  input.addEventListener("keydown", (e) => {
    if (e.key === "ArrowDown" && shown.length) active = Math.min(active + 1, shown.length - 1);
    else if (e.key === "ArrowUp" && shown.length) active = Math.max(active - 1, -1);
    else if (e.key === "Enter") {
      if (active >= 0) add(shown[active]);
      else if (input.value.trim()) add(input.value);
      else save();
    } else if (e.key === "Backspace" && input.value === "" && chosen.length) {
      chosen.pop();
      touched = true;
      renderChips();
    } else return;
    e.preventDefault();
    renderSuggestions();
  });

  pop.append(
    el("strong", {}, spec.label),
    chips,
    input,
    list,
    field === "genre" ? el("p", { class: "hint" }, "Lowercase a-z and -, words separated by single spaces.") : null,
    mixed ? el("p", { class: "hint" }, "Tracks differ. Values chosen here replace them on every track.") : null,
    error,
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "button", onclick: save }, "Save"),
      el("button", { class: "btn", type: "button", onclick: () => close(true) }, "Cancel")));
  renderChips();
  input.focus();
  knownValues(field).then((values) => {
    known = values;
    renderSuggestions();
  }).catch(handleError);
  return close;
}

/* Yes / no for compilation. onSave(true|false). */
function boolEditor(anchor, { current, onSave, onDone }) {
  const { pop, close } = floatingPopup(anchor, "Compilation", (refocus) => onDone(refocus));
  const choose = async (v) => {
    if (await onSave(v)) close(true);
  };
  const yes = el("button", { class: current === "1" ? "btn go" : "btn", type: "button",
                             onclick: () => choose(true) }, "Yes (various artists)");
  const no = el("button", { class: current === "1" ? "btn" : "btn go", type: "button",
                            onclick: () => choose(false) }, "No");
  pop.append(el("strong", {}, "Compilation"),
             el("div", { class: "actions" }, yes, no,
                el("button", { class: "btn", type: "button", onclick: () => close(true) }, "Cancel")));
  (current === "1" ? yes : no).focus();
  return close;
}

/*
 * A table cell for one tag. shown: the value (null if mixed), mixed: tracks
 * differ, edited: a change is pending, locked: why it can not be edited
 * (or nothing). save(value) queues it (string, or true/false for
 * compilation) and resolves true when done; errors are shown. key: to
 * find the cell again after the row is drawn anew.
 */
function tagCell({ field, shown, mixed, edited, locked, missing, key, save }) {
  const spec = FIELDS[field];
  const td = el("td", { class: [`col-${field}`, edited ? "edited" : "", missing ? "missing" : ""]
                                .filter(Boolean).join(" ") });
  const text = mixed ? "mixed" : missing ? "missing" : showValue(field, shown);
  const button = el("button", {
    type: "button", class: mixed || (!shown && !missing) ? "cell empty" : "cell",
    "data-key": key, disabled: Boolean(locked), title: locked || null,
    "aria-label": `${spec.label}: ${text}${edited ? " (changed, pending)" : ""}`,
  }, text);
  td.append(button);

  const run = async (value) => {
    try {
      await save(value);
      return true;
    } catch (err) {
      if (!(err instanceof ApiError && err.status === 401 && handleError(err))) {
        setStatus(`${spec.label}: ${err.message}`, true);
      }
      return false;
    }
  };

  function editText() {
    const initial = mixed ? "" : shown ?? "";
    const input = el("input", { class: "cell-input", maxlength: MAX_VALUE, value: initial,
                                placeholder: mixed ? "Differs: leave empty to keep" : spec.placeholder || "",
                                "aria-label": spec.label });
    let done = false;
    const finish = async (keep) => {
      if (done) return;
      done = true;
      const v = input.value.trim();
      if (keep && v !== initial && !(mixed && v === "")) {
        input.disabled = true;
        if (!(await run(v))) {
          done = false;
          input.disabled = false;
          input.focus();
          return;
        }
      } else {
        input.replaceWith(button);
        editing = false;
        button.focus();
      }
    };
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter") { e.preventDefault(); finish(true); }
      else if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); finish(false); }
    });
    input.addEventListener("blur", () => finish(true));
    button.replaceWith(input);
    input.focus();
    input.select();
  }

  let editing = false;
  button.addEventListener("click", () => {
    if (editing) return;
    editing = true;
    const back = (refocus) => {
      editing = false;
      if (refocus && button.isConnected) button.focus();
    };
    if (spec.kind === "list") {
      listEditor(button, { field, initial: mixed ? [] : splitList(shown), mixed,
                           onSave: run, onDone: back });
    } else if (spec.kind === "bool") {
      boolEditor(button, { current: mixed ? null : shown, onSave: run, onDone: back });
    } else {
      editText();
    }
  });
  return td;
}

const BUSY_LOCK = "A scan or write is running: editing is possible again when it is done.";
const MULTI_LOCK = "Several values in a file: nylm does not change those yet.";

/* Focuses the cell with this key again after its row was drawn anew,
 * unless the focus has moved on to something else meanwhile. */
function refocusCell(container, key) {
  const now = document.activeElement;
  if (now && now !== document.body && !container.contains(now)) return;
  const b = container.querySelector(`[data-key="${CSS.escape(key)}"]`);
  if (b) b.focus();
}

/* ---- albums table ------------------------------------------------------- */

/* Queues an album-wide change and returns the album's row as it is now. */
async function saveAlbumField(albumId, field, value) {
  const result = await api("POST", "/api/music/album/save",
                           { id: albumId, album: { [field]: value }, tracks: [] });
  if (field in FIELDS && FIELDS[field].kind === "list") valueCache.delete(field);
  const [row] = await api("GET", `/api/music/albums?id=${albumId}`);
  updatePendingCount().catch(handleError);
  if (result.queued || result.dropped) {
    setStatus(result.queued ? `Queued: ${FIELDS[field].label.toLowerCase()} for ` +
                              `${row.next.album || row.dir}` : "Back to the files' value");
  }
  return row;
}

/* Whether field's value in the album row will change. */
function albumFieldEdited(a, f) {
  return a.now[f] !== a.next[f] || a.now_mixed.includes(f) !== a.next_mixed.includes(f);
}

/*
 * One album as a table row (planned values, editable). options.readOnly:
 * nothing can be edited; compact: only the tag cells; onSaved(key): after
 * a change was queued (else the row is drawn anew).
 */
function albumRow(a, options) {
  const tr = el("tr", { class: albumRowClass(a) });
  const render = () => {
    tr.className = albumRowClass(a);
    tr.replaceChildren(
      ...ALBUM_COLUMNS.map((f) => tagCell({
        field: f,
        shown: a.next[f],
        mixed: a.next_mixed.includes(f),
        edited: albumFieldEdited(a, f),
        locked: options.readOnly ? BUSY_LOCK : null,
        key: `${a.id}:${f}`,
        save: async (value) => {
          a = await saveAlbumField(a.id, f, value);
          if (options.onSaved) {
            await options.onSaved(`${a.id}:${f}`);
            return;
          }
          render();
          refocusCell(tr, `${a.id}:${f}`);
        },
      })),
      options.compact ? null : el("td", { class: "col-tracks num" }, String(a.tracks)),
      options.compact ? null : el("td", { class: "col-dir path muted" }, a.dir || "(top folder)"),
      options.compact ? null : el("td", { class: "col-open" }, navButton("Open", `#/album/${a.id}`)));
  };
  render();
  return tr;
}

function albumRowClass(a) {
  return [a.album_pending ? "album-edits" : "", a.track_pending ? "track-edits" : ""]
    .filter(Boolean).join(" ");
}

function albumHead(...extra) {
  return el("thead", {}, el("tr", {},
    ...extra.slice(0, 1),
    ...ALBUM_COLUMNS.map((f) => el("th", { scope: "col", class: `col-${f}` }, FIELDS[f].label)),
    ...extra.slice(1)));
}

/* Words to search for in an album: its tags (now and planned) and folder. */
function albumText(a) {
  return [a.dir, ...ALBUM_COLUMNS.flatMap((f) => [a.now[f], a.next[f]])]
    .filter(Boolean).join(" ").toLowerCase();
}

function legend() {
  return el("p", { class: "legend hint" },
    el("span", { class: "mark album-edits" }, "Album changed"),
    el("span", { class: "mark track-edits" }, "Tracks changed"),
    el("span", { class: "mark edited-cell" }, "Changed value"),
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
  const filter = el("input", { type: "search", "aria-label": "Filter albums",
                               placeholder: "Search album, artist, genre, composer or folder" });
  const body = el("tbody");
  const count = el("span", { class: "count" });
  const readOnly = Boolean(o.busy);
  const rows = new Map(albums.map((a) => [a.id, albumRow(a, { readOnly })]));
  const show = () => {
    const q = filter.value.trim().toLowerCase();
    const shown = albums.filter((a) => !q || albumText(a).includes(q));
    count.textContent = shown.length;
    body.replaceChildren(...shown.map((a) => rows.get(a.id)));
  };
  filter.addEventListener("input", show);
  show();
  return shell("albums", o,
    libraryCard(o),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Albums ", count)),
      albums.length
        ? [filter,
           readOnly ? el("p", { class: "warn" }, "A scan or write is running: editing is " +
                                                 "possible again when it is done.") : null,
           legend(),
           el("div", { class: "grid-wrap" },
             el("table", { class: "grid" },
               albumHead(null, el("th", { scope: "col" }, "Tracks"),
                         el("th", { scope: "col" }, "Folder"),
                         el("th", { scope: "col" }, el("span", { class: "visually-hidden" }, "Open"))),
               body))]
        : el("p", { class: "empty" }, "No albums yet. Scan the library.")));
}

/* ---- one album ---------------------------------------------------------- */

/* A track's value for field f as it will be: the pending change, else the
 * file's value ("" when absent). */
function planned(t, f) {
  return f in t.pending ? t.pending[f] : t[f] ?? "";
}

async function albumPage(id) {
  const [o, rowList, a] = await Promise.all([
    api("GET", "/api/music"),
    api("GET", `/api/music/albums?id=${id}`),
    api("GET", `/api/music/album?id=${id}`),
  ]);
  const row = rowList[0];
  const readOnly = Boolean(o.busy);
  const seconds = a.tracks.reduce((sum, t) => sum + t.seconds, 0);
  const covers = a.tracks.filter((t) => t.pictures > 0).length;
  const missing = a.tracks.filter((t) => planned(t, "tracknumber") === "").length;

  /* After any change the page is drawn again (an album-wide composer also
   * shows on the tracks), back on the same cell. */
  const redraw = async (key) => {
    await refresh();
    refocusCell(app, key);
  };
  const albumTable = el("table", { class: "grid one" },
    albumHead(), el("tbody", {}, albumRow(row, { readOnly, compact: true, onSaved: redraw })));

  const saveTrack = async (t, f, value) => {
    const result = await api("POST", "/api/music/album/save",
                             { id: a.id, album: {}, tracks: [{ id: t.id, [f]: value }] });
    if (FIELDS[f].kind === "list") valueCache.delete(f);
    setStatus(result.queued ? `Queued: ${FIELDS[f].label.toLowerCase()} of ${t.file}`
                            : result.dropped ? "Back to the file's value" : "");
    updatePendingCount().catch(handleError);
    await redraw(`${t.id}:${f}`);
  };

  const trackRows = a.tracks.map((t) => el("tr",
    { class: Object.keys(t.pending).length ? "track-edits" : "" },
    ...TRACK_COLUMNS.map((f) => {
      const value = planned(t, f);
      return tagCell({
        field: f,
        shown: value === "" ? null : value,
        mixed: false,
        edited: f in t.pending,
        locked: readOnly ? BUSY_LOCK : t.locked.includes(f) ? MULTI_LOCK : null,
        missing: f === "tracknumber" && value === "",
        key: `${t.id}:${f}`,
        save: (v) => saveTrack(t, f, v),
      });
    }),
    el("td", { class: "path muted" }, t.file),
    el("td", { class: "num muted" }, duration(t.seconds))));

  return shell("albums", o,
    el("div", { class: "actions" }, navButton("All albums", "#/albums")),
    el("header", { class: "section album-head" },
      el("h2", {}, row.next_mixed.includes("album") ? "Several album names"
                                                     : row.next.album ?? "No album name"),
      el("p", { class: "muted path" }, a.dir || "(the top folder)"),
      el("p", { class: "muted" },
         `${plural(a.tracks.length, "track", "tracks")} · ${duration(seconds)} · ` +
         `cover in ${covers} of ${a.tracks.length}`)),
    readOnly ? el("p", { class: "warn" }, "A scan or write is running: editing is possible " +
                                          "again when it is done.") : null,
    legend(),
    el("section", { class: "section" },
      el("h2", {}, "Whole album"),
      el("p", { class: "hint" }, "A value set here is queued for every track of the album."),
      el("div", { class: "grid-wrap" }, albumTable)),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Tracks ", el("span", { class: "count" }, a.tracks.length))),
      missing ? el("p", { class: "warn" },
                   `${plural(missing, "track has", "tracks have")} no track number: fill ` +
                   "them in before other changes to this album can be queued.") : null,
      el("p", { class: "hint" }, "A missing disc number is queued as 1/1 with the album's " +
                                 "next change."),
      el("div", { class: "grid-wrap" },
        el("table", { class: "grid tracks" },
          el("thead", {}, el("tr", {},
            ...TRACK_COLUMNS.map((f) => el("th", { scope: "col", class: `col-${f}` }, FIELDS[f].label)),
            el("th", { scope: "col" }, "File"),
            el("th", { scope: "col" }, "Length"))),
          el("tbody", {}, trackRows)))));
}

/* ---- changes ------------------------------------------------------------ */

/* A row of an album's values, not editable: now (files) or next (planned). */
function valuesRow(a, which, extra) {
  const values = a[which];
  const mixed = a[`${which}_mixed`];
  return el("tr", { class: which === "next" ? "new-values" : "old-values" },
    el("th", { scope: "row", class: "which" }, which === "next" ? "New" : "Now"),
    ...ALBUM_COLUMNS.map((f) => {
      const changed = albumFieldEdited(a, f);
      return el("td", { class: [`col-${f}`, changed && which === "next" ? "edited" : "",
                                changed && which === "now" ? "replaced" : ""].filter(Boolean).join(" ") },
        el("span", { class: mixed.includes(f) ? "cell-text empty" : "cell-text" },
           mixed.includes(f) ? "mixed" : showValue(f, values[f])));
    }),
    ...extra);
}

/* One album with pending changes: two rows (now, new), a Tracks cell that
 * can show the track changes underneath, and Discard. */
function changedAlbum(a, trackChanges) {
  const detail = el("tr", { class: "track-detail", hidden: true },
    el("td", { colspan: String(ALBUM_COLUMNS.length + 3) },
      el("ul", { class: "changes" }, trackChanges.map((c) =>
        el("li", {},
          el("span", { class: "path muted" }, `${c.path.split("/").pop()} · `),
          el("span", { class: "muted" }, `${FIELDS[c.field].label}: `),
          el("span", { class: "old" }, showValue(c.field, c.old)),
          " → ",
          el("strong", {}, c.new === "" ? "(removed)" : showValue(c.field, c.new)))))));
  const toggle = trackChanges.length
    ? el("button", { class: "btn", type: "button", "aria-expanded": "false", onclick: () => {
        detail.hidden = !detail.hidden;
        toggle.setAttribute("aria-expanded", String(!detail.hidden));
        toggle.textContent = detail.hidden ? "Show" : "Hide";
      } }, "Show")
    : null;
  const discard = el("button", { class: "btn danger", type: "button", onclick: async () => {
    if (!sure(`Discard every pending change of ${a.next.album || a.dir}?`)) return;
    discard.disabled = true;
    try {
      const r = await api("POST", "/api/music/changes/discard", { album_id: a.id });
      setStatus(`Discarded ${plural(r.cancelled, "change", "changes")}`);
      refresh();
    } catch (err) {
      handleError(err);
      discard.disabled = false;
    }
  } }, "Discard");
  const tracksCell = el("td", { class: "col-tracks", rowspan: "2" },
    trackChanges.length ? el("div", { class: "stack-tight" },
                             el("span", { class: "track-count" },
                                plural(trackChanges.length, "track change", "track changes")),
                             toggle)
                        : el("span", { class: "muted" }, "—"));
  const actions = el("td", { class: "col-open", rowspan: "2" },
    el("div", { class: "stack-tight" }, navButton("Open", `#/album/${a.id}`), discard));
  return el("tbody", { class: albumRowClass(a) },
    valuesRow(a, "now", [tracksCell, actions]),
    valuesRow(a, "next", []),
    detail);
}

const STATE_TEXT = { done: "Done", warning: "Done, with a warning", failed: "Failed" };

/* One written (or failed) change. Colour only for what needs a look:
 * rose failed, peach warning. */
function historyItem(c) {
  const tone = c.state === "failed" ? "late" : c.state === "warning" ? "today" : "";
  return el("li", { class: "card stack history" },
    el("header", {},
      el("span", { class: tone ? `due ${tone}` : "muted" }, STATE_TEXT[c.state]),
      el("span", { class: "muted" }, c.finished ? showTime(c.finished) : "")),
    el("strong", { class: "path" }, c.path),
    el("ul", { class: "changes" },
      el("li", {},
        el("span", { class: "muted" }, `${FIELDS[c.field].label}: `),
        el("span", { class: "old" }, showValue(c.field, c.old)),
        " → ",
        el("strong", {}, c.new === "" ? "(removed)" : showValue(c.field, c.new)))),
    c.note ? el("p", { class: "note" }, c.note) : null);
}

async function changesPage() {
  const [o, ch, albums] = await Promise.all([api("GET", "/api/music"),
                                             api("GET", "/api/music/changes"),
                                             api("GET", "/api/music/albums")]);
  const changed = albums.filter((a) => a.album_pending || a.track_pending);
  const trackChanges = (albumId) => ch.pending.filter((c) =>
    c.album_id === albumId && !ALBUM_COLUMNS.includes(c.field));
  const files = new Set(ch.pending.map((c) => c.path)).size;
  const write = serviceForm("/api/music/write",
    `Writes ${plural(ch.pending.length, "change", "changes")} into ` +
    `${plural(files, "file", "files")}. Each file is checked first (the value it was ` +
    "queued against, a valid new value) and read back after; the result of each change is " +
    "listed in the history.",
    "Write changes", "Writing started");
  const text = el("p", {}, busyText(o));
  const panel = el("section", { class: "card stack" },
    el("h2", {}, "Pending ", el("span", { class: "count" }, ch.pending.length)),
    o.busy ? text : null,
    !o.available ? el("p", { class: "warn" },
                      "The music folder is not available (is the drive mounted?): nothing can " +
                      "be written now.") : null,
    ch.pending.length === 0
      ? el("p", { class: "muted" }, "Nothing is queued. Edit albums to queue changes.")
      : o.busy || !o.available ? null
      : el("div", { class: "actions" },
          el("button", { class: "btn go", type: "button", onclick: () => write.open() },
             "Write changes…")),
    write);
  watchBusy(o, panel, text);

  return shell("changes", o,
    panel,
    changed.length
      ? el("section", { class: "section" },
          legend(),
          el("div", { class: "grid-wrap" },
            el("table", { class: "grid changes-grid" },
              albumHead(el("th", { scope: "col" }, el("span", { class: "visually-hidden" }, "Values")),
                        el("th", { scope: "col" }, "Tracks"),
                        el("th", { scope: "col" }, el("span", { class: "visually-hidden" }, "Actions"))),
              changed.map((a) => changedAlbum(a, trackChanges(a.id))))))
      : null,
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "History ", el("span", { class: "count" }, ch.history.length))),
      ch.history.length
        ? el("ul", { class: "list cols" }, ch.history.map(historyItem))
        : el("p", { class: "empty" }, "Nothing written yet.")));
}

/* ---- routing ------------------------------------------------------------ */

/* ["album", "3"] -> that album, etc. Anything else goes to the albums. */
function route(parts) {
  const [section, rawId] = parts;
  const id = Number(rawId);
  if (section === "album" && Number.isInteger(id) && id > 0) return albumPage(id);
  if (section === "albums" && rawId === undefined) return albumsPage();
  if (section === "changes" && rawId === undefined) return changesPage();
  go("#/albums");
  return null;
}

startApp(route);
