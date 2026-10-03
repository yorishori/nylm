"use strict";

/*
 * Music app (/music/). Sections by URL hash:
 *   #/albums      every album (a folder of the library), and the library scan
 *   #/album/ID    one album: change its tags; the changes are queued
 *   #/changes     queued changes (cancel, or write them), and the history
 *
 * The server never touches the files: it keeps a cache of their tags and a
 * queue of changes. Two services do the work, one at a time: the scan
 * (files -> cache) and the write (queue -> files). Both are started here,
 * with the password again. While one runs, nothing can be queued.
 */

const ALBUM_FIELDS = [
  ["album", "Album"],
  ["albumartist", "Album artist"],
  ["genre", "Genre"],
  ["date", "Date"],
];
const TRACK_FIELDS = [
  ["discnumber", "Disc"],
  ["tracknumber", "No."],
  ["title", "Title"],
  ["artist", "Artist"],
];
const LABELS = Object.fromEntries([...ALBUM_FIELDS, ...TRACK_FIELDS,
                                   ["compilation", "Compilation"]]);
const MAX_VALUE = 500; /* bytes; the server checks the exact limit */
const POLL_MS = 3000;

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
  if (v == null || v === "") return "(none)";
  if (field === "compilation") return v === "1" ? "yes" : v;
  return v;
}

function shell(active, overview, ...content) {
  const tabs = [
    ["albums", "Albums"],
    ["changes", overview.pending ? `Changes (${overview.pending})` : "Changes"],
  ];
  return el("section", {},
    el("header", { class: "app-head" },
      el("h1", {}, "Music"),
      el("nav", { class: "tabs", "aria-label": "Music sections" },
        tabs.map(([key, label]) =>
          el("a", { href: `#/${key}`, "aria-current": key === active ? "page" : null },
             label)))),
    ...content);
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
        setStatus(o.busy === "write" ? "Writing finished: see the results below"
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

/* ---- albums ------------------------------------------------------------- */

function albumCard(a) {
  const title = a.album != null ? a.album
              : a.mixed.includes("album") ? "Several album names" : "No album name";
  const artist = a.albumartist != null ? a.albumartist
               : a.mixed.includes("albumartist") ? "Several album artists" : "No album artist";
  return el("li", { class: "card album-card" },
    el("h3", {}, title),
    el("p", {}, artist),
    el("p", { class: "muted" },
       [a.date, plural(a.tracks, "track", "tracks"),
        a.with_art < a.tracks ? `cover in ${a.with_art} of ${a.tracks}` : "cover in all"]
         .filter(Boolean).join(" · ")),
    el("p", { class: "muted path" }, a.dir || "(the top folder)"),
    a.mixed.length
      ? el("p", { class: "hint" },
           `Differs between tracks: ${a.mixed.map((f) => LABELS[f].toLowerCase()).join(", ")}`)
      : null,
    a.pending ? el("p", { class: "pending-note" }, `${plural(a.pending, "change", "changes")} pending`)
              : null,
    el("div", { class: "actions" }, navButton("Open", `#/album/${a.id}`)));
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
                               placeholder: "Filter by album, artist or folder" });
  const list = el("ul", { class: "list cols" });
  const count = el("span", { class: "count" });
  const show = () => {
    const q = filter.value.trim().toLowerCase();
    const shown = albums.filter((a) => !q ||
      [a.album, a.albumartist, a.dir].some((v) => v && v.toLowerCase().includes(q)));
    count.textContent = shown.length;
    list.replaceChildren(...shown.map(albumCard));
  };
  filter.addEventListener("input", show);
  show();
  return shell("albums", o,
    libraryCard(o),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Albums ", count)),
      albums.length ? [filter, list]
                    : el("p", { class: "empty" }, "No albums yet. Scan the library.")));
}

/* ---- one album ---------------------------------------------------------- */

/* A track's value for field f as it will be: the pending change, else the
 * file's value ("" when absent). */
function planned(t, f) {
  return f in t.pending ? t.pending[f] : t[f] ?? "";
}

/* What the album's tracks will have for field f: shared or not, locked,
 * whether a change is pending. */
function fieldState(tracks, f) {
  const values = tracks.map((t) => planned(t, f));
  const same = values.every((v) => v === values[0]);
  return {
    same,
    shared: same ? values[0] : null,
    distinct: [...new Set(values.map((v) => showValue(f, v)))],
    locked: tracks.some((t) => t.locked.includes(f)),
    pending: tracks.some((t) => f in t.pending),
  };
}

/*
 * A text input for a tag. read() gives the new value, or null if it is
 * still what the track has (the file's value or the pending one). fill:
 * a value shown in place of an empty one, so it is queued like a change.
 */
function tagInput(label, initial, { locked, placeholder, pending, fill }) {
  const input = el("input", { maxlength: MAX_VALUE, value: fill || initial, placeholder,
                              disabled: Boolean(locked), "aria-label": label,
                              class: pending || fill ? "pending" : null });
  input.read = () => (locked || input.value === initial ? null : input.value.trim());
  return input;
}

const LOCKED_HINT = "Several values in some files: nylm does not change those yet.";
const GENRE_HINT = "Lowercase a-z, - and single spaces; several separated by \"; \" " +
                   "(hip hop; pop-punk).";
const DISC_DEFAULT = "1/1";

/* The album-wide inputs. read() gives {field: value} of the changed ones. */
function albumInputs(tracks) {
  const inputs = [];
  const fields = ALBUM_FIELDS.map(([f, label]) => {
    const s = fieldState(tracks, f);
    const input = tagInput(label, s.same ? s.shared : "", {
      locked: s.locked,
      pending: s.pending,
      placeholder: !s.same ? "Leave empty to keep"
                 : f === "date" ? "YYYY or YYYY-MM-DD"
                 : f === "genre" ? "hip hop; pop-punk" : "",
    });
    inputs.push([f, input, s.same]);
    const hints = [
      s.locked ? LOCKED_HINT : null,
      !s.locked && !s.same ? `Differs between tracks: ${s.distinct.slice(0, 4).join(", ")}` +
                             (s.distinct.length > 4 ? ", …" : "") + ". Leave empty to keep each."
                           : null,
      !s.locked && s.same && s.pending ? "Includes a pending change." : null,
      f === "genre" ? GENRE_HINT : null,
    ].filter(Boolean);
    return field(label, input, hints.length ? hints.join(" ") : null);
  });

  const comp = fieldState(tracks, "compilation");
  const compInitial = comp.same ? (comp.shared === "1" ? "yes" : "no") : "keep";
  const compChoice = comp.locked ? null : dropdown({
    label: "Compilation",
    value: compInitial,
    options: [
      ...(comp.same ? [] : [{ value: "keep", label: "Differs between tracks: keep" }]),
      { value: "no", label: "No" },
      { value: "yes", label: "Yes (various artists)" },
    ],
  });
  fields.push(comp.locked ? field("Compilation", el("p", { class: "muted" }, LOCKED_HINT))
                          : field("Compilation", compChoice,
                                  comp.pending ? "Includes a pending change." : null));

  return {
    fields,
    read() {
      const out = {};
      for (const [f, input, same] of inputs) {
        const v = input.read();
        /* A differing field left empty is kept, not removed. */
        if (v !== null && (same || v !== "")) out[f] = v;
      }
      if (compChoice && compChoice.value !== compInitial) {
        out.compilation = compChoice.value === "yes";
      }
      return out;
    },
  };
}

/*
 * One track's row of inputs. read() gives {field: value} of the changed
 * ones; missing() is true while its track or disc number is empty (both
 * are required). A missing disc number is filled in as 1/1.
 */
function trackRow(t) {
  const filled = (f) => f === "discnumber" && planned(t, f) === "" && !t.locked.includes(f);
  const inputs = TRACK_FIELDS.map(([f, label]) =>
    [f, tagInput(label, planned(t, f), { locked: t.locked.includes(f), pending: f in t.pending,
                                         fill: filled(f) ? DISC_DEFAULT : null })]);
  const hint = (f) => t.locked.includes(f) ? LOCKED_HINT
                    : filled(f) ? "Filled in: the file has no disc number."
                    : f in t.pending ? `Pending (file has ${showValue(f, t[f])})`
                    : f === "tracknumber" && planned(t, f) === "" ? "Required: the file has none."
                    : null;
  const row = el("li", { class: "card track" },
    el("header", {},
      el("strong", { class: "path" }, t.file),
      el("span", { class: "muted" },
         [t.format.toUpperCase(), duration(t.seconds), t.pictures ? "cover" : "no cover"]
           .join(" · "))),
    el("div", { class: "track-fields" },
      inputs.map(([f, input]) => field(LABELS[f], input, hint(f)))));
  row.read = () => {
    const out = {};
    for (const [f, input] of inputs) {
      const v = input.read();
      if (v !== null) out[f] = v;
    }
    return out;
  };
  row.missing = () => inputs.some(([f, input]) =>
    (f === "tracknumber" || f === "discnumber") && !input.disabled && input.value.trim() === "");
  return row;
}

/*
 * What the request will do per track, as the server sees it:
 * [{track, changes: [[field, file value, new value, cancels]]}], where
 * cancels means the new value is the file's own (the pending change goes).
 */
function plannedChanges(tracks, albumChanges, trackChanges) {
  const plan = [];
  for (const t of tracks) {
    const changes = [];
    const wanted = { ...albumChanges, ...(trackChanges.get(t.id) || {}) };
    for (const [f, value] of Object.entries(wanted)) {
      const v = f === "compilation" ? (value ? "1" : "") : value;
      if (planned(t, f) === v) continue;
      changes.push([f, t[f], v, (t[f] ?? "") === v]);
    }
    if (changes.length) plan.push({ track: t, changes });
  }
  return plan;
}

function changeLine(field, before, after, cancels) {
  return el("li", {},
    el("span", { class: "muted" }, `${LABELS[field]}: `),
    el("span", { class: "old" }, showValue(field, before)),
    " → ",
    el("strong", {}, after === "" ? "(removed)" : showValue(field, after)),
    cancels ? el("span", { class: "muted" }, " (the file's own value: cancels the pending change)")
            : null);
}

async function albumPage(id) {
  const [o, a] = await Promise.all([api("GET", "/api/music"),
                                    api("GET", `/api/music/album?id=${id}`)]);
  const album = albumInputs(a.tracks);
  const rows = a.tracks.map(trackRow);
  const seconds = a.tracks.reduce((sum, t) => sum + t.seconds, 0);
  const covers = a.tracks.filter((t) => t.pictures > 0).length;
  const pending = a.tracks.reduce((sum, t) => sum + Object.keys(t.pending).length, 0);
  const title = fieldState(a.tracks, "album");

  const review = el("div", { hidden: true });
  const editor = el("div", { class: "stack" },
    pending ? el("p", { class: "pending-note" },
                 `${plural(pending, "change", "changes")} pending for this album: shown in ` +
                 "the fields below, written when you write the changes.") : null,
    el("section", { class: "raised stack" },
      el("h2", {}, "Whole album"),
      el("p", { class: "hint" }, "Set here, a tag is changed on every track of the album."),
      el("div", { class: "album-fields" }, album.fields)),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Tracks ", el("span", { class: "count" }, a.tracks.length))),
      el("ol", { class: "list" }, rows)),
    o.busy
      ? el("p", { class: "warn" }, "A scan or write is running; changes can be queued when " +
                                   "it is done.")
      : el("div", { class: "actions" },
          el("button", { class: "btn go", type: "button", onclick: showReview }, "Review changes")));

  function showReview() {
    const missing = a.tracks.filter((t, i) => rows[i].missing()).map((t) => t.file);
    if (missing.length) {
      setStatus(`Fill in the track and disc number of ${missing.slice(0, 3).join(", ")}` +
                (missing.length > 3 ? ` and ${missing.length - 3} more` : "") +
                ": every track needs both.", true);
      return;
    }
    const albumChanges = album.read();
    const trackChanges = new Map(rows.map((r, i) => [a.tracks[i].id, r.read()]));
    const plan = plannedChanges(a.tracks, albumChanges, trackChanges);
    if (plan.length === 0) {
      setStatus("Nothing to change");
      return;
    }
    const count = plan.reduce((sum, p) => sum + p.changes.length, 0);
    const body = {
      id: a.id,
      album: albumChanges,
      tracks: [...trackChanges].filter(([, c]) => Object.keys(c).length)
                               .map(([trackId, c]) => ({ id: trackId, ...c })),
    };
    const back = () => {
      review.hidden = true;
      editor.hidden = false;
    };
    review.replaceChildren(form({ class: "raised stack" }, async () => {
      const result = await api("POST", "/api/music/album/save", body);
      const parts = [];
      if (result.queued) parts.push(`queued ${plural(result.queued, "change", "changes")}`);
      if (result.dropped) parts.push(`cancelled ${plural(result.dropped, "pending change", "pending changes")}`);
      const text = parts.join(", ") || "nothing changed";
      setStatus(text[0].toUpperCase() + text.slice(1) +
                (result.queued ? ". Write them from Changes." : ""));
      refresh();
    },
      el("h2", {}, `Review: ${plural(count, "change", "changes")} in ` +
                   plural(plan.length, "file", "files")),
      el("p", { class: "hint" },
         "These changes are queued, not written yet. Write them from Changes; the files are " +
         "checked before and after each change."),
      el("ul", { class: "list" }, plan.map(({ track, changes }) =>
        el("li", { class: "card" },
          el("strong", { class: "path" }, track.file),
          el("ul", { class: "changes" }, changes.map((c) => changeLine(...c)))))),
      el("div", { class: "actions" },
        el("button", { class: "btn go", type: "submit" },
           `Queue ${plural(count, "change", "changes")}`),
        el("button", { class: "btn", type: "button", onclick: back }, "Back to editing"))));
    editor.hidden = true;
    review.hidden = false;
    review.querySelector("h2").scrollIntoView({ block: "start" });
  }

  return shell("albums", o,
    el("div", { class: "actions" }, navButton("All albums", "#/albums")),
    el("header", { class: "section album-head" },
      el("h2", {}, title.same ? title.shared || "No album name" : "Several album names"),
      el("p", { class: "muted path" }, a.dir || "(the top folder)"),
      el("p", { class: "muted" },
         `${plural(a.tracks.length, "track", "tracks")} · ${duration(seconds)} · ` +
         `cover in ${covers} of ${a.tracks.length}`)),
    editor,
    review);
}

/* ---- changes ------------------------------------------------------------ */

async function cancelChanges(ids, what) {
  const result = await api("POST", "/api/music/changes/cancel", { ids });
  setStatus(`Cancelled ${plural(result.cancelled, "change", "changes")}${what}`);
  refresh();
}

/* A button that cancels the given pending changes (asks first if many). */
function cancelButton(label, ids, what) {
  const button = el("button", { class: "btn", type: "button", onclick: async () => {
    if (ids.length > 1 && !sure(`Cancel ${plural(ids.length, "pending change", "pending changes")}${what}?`)) {
      return;
    }
    button.disabled = true;
    try {
      await cancelChanges(ids, what);
    } catch (err) {
      handleError(err);
      button.disabled = false;
    }
  } }, label);
  return button;
}

/* The pending changes of one file. */
function pendingCard(path, rows) {
  const albumId = rows[0].album_id;
  return el("li", { class: "card stack" },
    el("strong", { class: "path" }, path),
    el("ul", { class: "changes" },
       rows.map((c) => changeLine(c.field, c.old, c.new, false))),
    el("div", { class: "actions" },
      albumId != null ? navButton("Open album", `#/album/${albumId}`) : null,
      cancelButton(rows.length > 1 ? "Cancel these" : "Cancel", rows.map((c) => c.id),
                   ` for ${path.split("/").pop()}`)));
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
    el("ul", { class: "changes" }, changeLine(c.field, c.old, c.new, false)),
    c.note ? el("p", { class: c.state === "failed" ? "note failed" : "note" }, c.note) : null);
}

async function changesPage() {
  const [o, ch] = await Promise.all([api("GET", "/api/music"),
                                     api("GET", "/api/music/changes")]);
  const byFile = new Map();
  for (const c of ch.pending) {
    if (!byFile.has(c.path)) byFile.set(c.path, []);
    byFile.get(c.path).push(c);
  }
  const write = serviceForm("/api/music/write",
    `Writes ${plural(ch.pending.length, "change", "changes")} into ` +
    `${plural(byFile.size, "file", "files")}. Each file is checked first (the value it was ` +
    "queued against, a valid new value) and read back after; the result of each change is " +
    "listed below.",
    "Write changes", "Writing started");
  const text = el("p", {}, busyText(o));
  const panel = el("section", { class: "card stack" },
    el("h2", {}, "Pending ", el("span", { class: "count" }, ch.pending.length)),
    o.busy ? text : null,
    !o.available ? el("p", { class: "warn" },
                      "The music folder is not available (is the drive mounted?): nothing can " +
                      "be written now.") : null,
    ch.pending.length === 0
      ? el("p", { class: "muted" }, "Nothing is queued. Change tags on an album to queue changes.")
      : o.busy || !o.available ? null
      : el("div", { class: "actions" },
          el("button", { class: "btn go", type: "button", onclick: () => write.open() },
             "Write changes…"),
          cancelButton("Cancel all", ch.pending.map((c) => c.id), "")),
    write);
  watchBusy(o, panel, text);

  return shell("changes", o,
    panel,
    byFile.size
      ? el("section", { class: "section" },
          el("ul", { class: "list cols" },
             [...byFile].map(([path, rows]) => pendingCard(path, rows))))
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
