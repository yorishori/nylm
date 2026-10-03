"use strict";

/*
 * Music app (/music/). Sections by URL hash:
 *   #/albums      every album (a folder of the library), and the library scan
 *   #/album/ID    one album: change its tags, review every change, write
 *
 * The files are the truth; the server keeps a cache of their tags, filled
 * by a scan. Nothing is written before the review lists every change.
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
const SCAN_POLL_MS = 3000;

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

function shell(...content) {
  return el("section", {},
    el("header", { class: "app-head" }, el("h1", {}, "Music")),
    ...content);
}

/* ---- library and scan --------------------------------------------------- */

function scanText(o) {
  const s = o.scan;
  switch (o.scan_state) {
    case "never":
      return "Never scanned.";
    case "running":
      return s && s.finished == null ? `Scanning… ${plural(s.files, "file", "files")} so far.`
                                     : "Scanning…";
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

/* The password form that starts a scan. */
function scanForm(onCancel) {
  const password = el("input", { type: "password", name: "password", required: true,
                                 autocomplete: "current-password" });
  const node = form({ class: "raised", hidden: true }, async () => {
    try {
      await api("POST", "/api/music/scan", { password: password.value });
    } finally {
      password.value = "";
    }
    setStatus("Scan started");
    refresh();
  },
    el("p", {}, "The scan reads new and changed files. It changes no file."),
    field("Password", password, "Starting a scan needs your password again."),
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "submit" }, "Start scan"),
      el("button", { class: "btn", type: "button", onclick: onCancel }, "Cancel")));
  node.focusFirst = () => password.focus();
  return node;
}

/* Counts, the last scan, and the scan button. While a scan runs it checks
 * back every few seconds and shows the page again when it is done. */
function libraryCard(o) {
  const text = el("p", {}, scanText(o));
  const scan = scanForm(() => { scan.hidden = true; });
  const card = el("section", { class: "card stack" },
    el("h2", {}, "Library"),
    el("p", {}, `${plural(o.albums, "album", "albums")}, ${plural(o.tracks, "track", "tracks")}`),
    o.available ? null
                : el("p", { class: "warn" },
                     "The music folder is not available (is the drive mounted?). The albums " +
                     "below are from the last scan; nothing can be changed now."),
    text,
    o.scan_state === "running" || !o.available ? null
      : el("div", { class: "actions" },
          el("button", { class: "btn", type: "button", onclick: () => {
            scan.hidden = false;
            scan.focusFirst();
          } }, "Scan library…")),
    scan);

  if (o.scan_state === "running") {
    const poll = async () => {
      if (!card.isConnected) return;
      try {
        const now = await api("GET", "/api/music");
        if (!card.isConnected) return;
        if (now.scan_state === "running") {
          text.textContent = scanText(now);
          setTimeout(poll, SCAN_POLL_MS);
        } else {
          setStatus(now.scan_state === "done" ? "Scan finished" : "Scan ended with problems",
                    now.scan_state !== "done");
          refresh();
        }
      } catch (err) {
        handleError(err);
      }
    };
    setTimeout(poll, SCAN_POLL_MS);
  }
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
    el("div", { class: "actions" }, navButton("Open", `#/album/${a.id}`)));
}

async function albumsPage() {
  const o = await api("GET", "/api/music");
  if (!o.configured) {
    return shell(el("div", { class: "empty" },
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
  return shell(
    libraryCard(o),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Albums ", count)),
      albums.length ? [filter, list]
                    : el("p", { class: "empty" }, "No albums yet. Scan the library.")));
}

/* ---- one album ---------------------------------------------------------- */

/* What the album's tracks have for field f: shared value or not, locked. */
function fieldState(tracks, f) {
  const values = tracks.map((t) => t[f]);
  const same = values.every((v) => v === values[0]);
  return {
    same,
    shared: same ? values[0] : null,
    distinct: [...new Set(values.map((v) => v ?? "(none)"))],
    locked: tracks.some((t) => t.locked.includes(f)),
  };
}

/*
 * A text input for a tag. read() gives the new value, or null if it was
 * not changed (the server then keeps what each file has).
 */
function tagInput(label, initial, { locked, placeholder }) {
  const input = el("input", { maxlength: MAX_VALUE, value: initial, placeholder,
                              disabled: Boolean(locked), "aria-label": label });
  input.read = () => (locked || input.value === initial ? null : input.value.trim());
  return input;
}

const LOCKED_HINT = "Several values in some files: nylm does not change those yet.";

/* The album-wide inputs. read() gives {field: value} of the changed ones. */
function albumInputs(tracks) {
  const inputs = [];
  const fields = ALBUM_FIELDS.map(([f, label]) => {
    const s = fieldState(tracks, f);
    const input = tagInput(label, s.same ? s.shared ?? "" : "", {
      locked: s.locked,
      placeholder: s.same ? (f === "date" ? "YYYY or YYYY-MM-DD" : "") : "Leave empty to keep",
    });
    inputs.push([f, input]);
    const hint = s.locked ? LOCKED_HINT
               : s.same ? null
               : `Differs between tracks: ${s.distinct.slice(0, 4).join(", ")}` +
                 (s.distinct.length > 4 ? ", …" : "") + ". Leave empty to keep each.";
    return field(label, input, hint);
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
                          : field("Compilation", compChoice));

  return {
    fields,
    read() {
      const out = {};
      for (const [f, input] of inputs) {
        const v = input.read();
        /* A differing field left empty is kept, not removed. */
        if (v !== null && (fieldState(tracks, f).same || v !== "")) out[f] = v;
      }
      if (compChoice && compChoice.value !== compInitial) {
        out.compilation = compChoice.value === "yes";
      }
      return out;
    },
  };
}

/* One track's row of inputs. read() gives {field: value} of the changed ones. */
function trackRow(t) {
  const inputs = TRACK_FIELDS.map(([f, label]) =>
    [f, tagInput(label, t[f] ?? "", { locked: t.locked.includes(f) })]);
  const row = el("li", { class: "card track" },
    el("header", {},
      el("strong", { class: "path" }, t.file),
      el("span", { class: "muted" },
         [t.format.toUpperCase(), duration(t.seconds), t.pictures ? "cover" : "no cover"]
           .join(" · "))),
    el("div", { class: "track-fields" },
      inputs.map(([f, input]) => field(LABELS[f], input,
                                       t.locked.includes(f) ? LOCKED_HINT : null))));
  row.read = () => {
    const out = {};
    for (const [f, input] of inputs) {
      const v = input.read();
      if (v !== null) out[f] = v;
    }
    return out;
  };
  return row;
}

/* The tags a change really changes, per track, as the server will see it:
 * [{track, changes: [[field, old, new]]}] with only real differences. */
function plannedChanges(tracks, albumChanges, trackChanges) {
  const plan = [];
  for (const t of tracks) {
    const changes = [];
    const wanted = { ...albumChanges, ...(trackChanges.get(t.id) || {}) };
    for (const [f, value] of Object.entries(wanted)) {
      const v = f === "compilation" ? (value ? "1" : "") : value;
      if ((t[f] ?? "") !== v) changes.push([f, t[f], v]);
    }
    if (changes.length) plan.push({ track: t, changes });
  }
  return plan;
}

function changeLine([f, before, after]) {
  return el("li", {},
    el("span", { class: "muted" }, `${LABELS[f]}: `),
    el("span", { class: "old" }, before ?? "(none)"),
    " → ",
    el("strong", {}, after === "" ? "(removed)" : after));
}

async function albumPage(id) {
  const [o, a] = await Promise.all([api("GET", "/api/music"),
                                    api("GET", `/api/music/album?id=${id}`)]);
  const album = albumInputs(a.tracks);
  const rows = a.tracks.map(trackRow);
  const seconds = a.tracks.reduce((sum, t) => sum + t.seconds, 0);
  const covers = a.tracks.filter((t) => t.pictures > 0).length;
  const title = fieldState(a.tracks, "album");

  const review = el("div", { hidden: true });
  const editor = el("div", { class: "stack" },
    el("section", { class: "raised stack" },
      el("h2", {}, "Whole album"),
      el("p", { class: "hint" }, "Set here, a tag is written to every track of the album."),
      el("div", { class: "album-fields" }, album.fields)),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Tracks ", el("span", { class: "count" }, a.tracks.length))),
      el("ol", { class: "list" }, rows)),
    o.available
      ? el("div", { class: "actions" },
          el("button", { class: "btn go", type: "button", onclick: showReview }, "Review changes"))
      : el("p", { class: "warn" }, "The music folder is not available: nothing can be changed."));

  function showReview() {
    const albumChanges = album.read();
    const trackChanges = new Map(rows.map((r, i) => [a.tracks[i].id, r.read()]));
    const plan = plannedChanges(a.tracks, albumChanges, trackChanges);
    if (plan.length === 0) {
      setStatus("Nothing to change");
      return;
    }
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
      setStatus(`Wrote ${plural(result.written, "file", "files")}`);
      refresh();
    },
      el("h2", {}, `Review: ${plural(plan.length, "file", "files")} will change`),
      el("p", { class: "hint" },
         "Each file is copied, changed and checked first; only if every file passes are " +
         "they replaced. Nothing else in the files changes."),
      el("ul", { class: "list" }, plan.map(({ track, changes }) =>
        el("li", { class: "card" },
          el("strong", { class: "path" }, track.file),
          el("ul", { class: "changes" }, changes.map(changeLine))))),
      el("div", { class: "actions" },
        el("button", { class: "btn go", type: "submit" },
           `Write ${plural(plan.length, "file", "files")}`),
        el("button", { class: "btn", type: "button", onclick: back }, "Back to editing"))));
    editor.hidden = true;
    review.hidden = false;
    review.querySelector("h2").scrollIntoView({ block: "start" });
  }

  return shell(
    el("div", { class: "actions" }, navButton("All albums", "#/albums")),
    el("header", { class: "section album-head" },
      el("h2", {}, title.same ? title.shared ?? "No album name" : "Several album names"),
      el("p", { class: "muted path" }, a.dir || "(the top folder)"),
      el("p", { class: "muted" },
         `${plural(a.tracks.length, "track", "tracks")} · ${duration(seconds)} · ` +
         `cover in ${covers} of ${a.tracks.length}`)),
    editor,
    review);
}

/* ---- routing ------------------------------------------------------------ */

/* ["album", "3"] -> that album; anything else goes to the albums. */
function route(parts) {
  const [section, rawId] = parts;
  const id = Number(rawId);
  if (section === "album" && Number.isInteger(id) && id > 0) return albumPage(id);
  if (section === "albums" && rawId === undefined) return albumsPage();
  go("#/albums");
  return null;
}

startApp(route);
