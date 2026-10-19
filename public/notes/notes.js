"use strict";

/*
 * Notes app (/notes/). Sections by URL hash:
 *   #/notes   quick reminders, then the notes, shown by tag and found by text
 *   #/tags    the tags: add, rename, recolour, delete
 *   #/stats   a few numbers about it all
 *
 * Everything is kept on this device first (localGet / localSet, key
 * "notes"), so the app works without the server. Each change waits in
 * state.pending until a sync (POST /api/notes/sync) sends it; the same
 * sync brings what changed elsewhere. It runs when a page is shown (soon
 * after each change), when the device is back online and when the app
 * comes back to the screen. src/api_notes.c says what happens when two
 * devices changed the same thing.
 */

const REMINDER_MAX = 200;        /* bytes, as on the server */
const NOTE_MAX = 20000;
const TAG_MAX = 50;
const NOTE_TAGS_MAX = 20;
const CHANGES_MAX = 500;         /* changes in one sync */
const BATCH_BYTES = 900 * 1024;  /* a sync's body stays under the server's 1 MiB */
const SYNC_DELAY_MS = 1500;
const SYNC_ROUNDS = 100;         /* requests in one sync, at most */

/* ---- the data on this device ------------------------------------------ */

/*
 * {since: the last rev synced, reminders / notes / tags: {id: the row as
 * the server sends it, without deleted}, pending: {"kind:id": {seq,
 * change}}, seq, synced: time of the last sync (ms) or null, unsynced:
 * how many changes wait}
 */
let state = null;

function emptyState() {
  return { since: 0, reminders: {}, notes: {}, tags: {}, pending: {}, seq: 0, synced: null,
           unsynced: 0 };
}

async function loadState() {
  state = (await localGet("notes")) || emptyState();
}

async function saveState() {
  state.unsynced = Object.keys(state.pending).length;
  await localSet("notes", state);
}

/* A new id, made here: 32 random lowercase hex characters. */
function newId() {
  const bytes = crypto.getRandomValues(new Uint8Array(16));
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

function nowSeconds() {
  return Math.floor(Date.now() / 1000);
}

function utf8Length(s) {
  return new TextEncoder().encode(s).length;
}

/*
 * Text as the server takes it: tabs as four spaces, line ends as "\n" (or
 * spaces, for one line), no other control characters.
 */
function cleanText(s, multiline) {
  let t = s.replace(/\r\n?/g, "\n").replace(/\t/g, "    ");
  if (t.toWellFormed) t = t.toWellFormed();
  t = t.replace(/[\u0000-\u0009\u000b-\u001f\u007f-\u009f]/g, "");
  return multiline ? t : t.replace(/\n/g, " ");
}

/* Keeps a change on this device (it waits for a sync) and shows it. */
async function record(kind, id, change) {
  state.seq++;
  state.pending[`${kind}:${id}`] = { seq: state.seq, change: { kind, id, ...change } };
  await saveState();
  refresh();
}

/* The rev a note's next change starts from: that of its waiting change,
 * or of the note as last synced (0: new). */
function noteBase(id) {
  const waiting = state.pending[`note:${id}`];
  if (waiting) return waiting.change.base;
  return state.notes[id] ? state.notes[id].rev : 0;
}

async function addReminder(text) {
  const r = { id: newId(), text, created: nowSeconds(), rev: 0 };
  state.reminders[r.id] = r;
  await record("reminder", r.id, { deleted: false, text, created: r.created });
}

async function removeReminder(id) {
  delete state.reminders[id];
  await record("reminder", id, { deleted: true });
}

/* Saves a note: a new one (id null) or a change to one. */
async function saveNote(id, text, tags) {
  const old = id ? state.notes[id] : null;
  const base = id ? noteBase(id) : 0;
  const t = nowSeconds();
  const note = { id: id || newId(), text, tags, created: old ? old.created : t, updated: t,
                 rev: old ? old.rev : 0 };
  state.notes[note.id] = note;
  await record("note", note.id, { base, deleted: false, text, tags, created: note.created,
                                  updated: t });
}

async function deleteNote(id) {
  const base = noteBase(id);
  delete state.notes[id];
  await record("note", id, { base, deleted: true });
}

async function saveTag(id, name, color) {
  const old = id ? state.tags[id] : null;
  const tag = { id: id || newId(), name, color, rev: old ? old.rev : 0 };
  state.tags[tag.id] = tag;
  await record("tag", tag.id, { deleted: false, name, color });
}

/* A deleted tag's id may stay on notes: it is skipped wherever shown. */
async function deleteTag(id) {
  delete state.tags[id];
  await record("tag", id, { deleted: true });
}

/* ---- sync ------------------------------------------------------------- */

let syncRun = null;    /* the sync running, if any */
let syncAgain = false; /* one more after it */
let syncTimer = null;
let firstSync = true;
let staleView = false; /* changes from elsewhere not shown yet (someone is typing) */

function scheduleSync() {
  clearTimeout(syncTimer);
  syncTimer = setTimeout(sync, SYNC_DELAY_MS);
}

/* The waiting changes for one request, oldest first: [{key, seq, change}]. */
function nextBatch() {
  const waiting = Object.entries(state.pending).sort((a, b) => a[1].seq - b[1].seq);
  const batch = [];
  let bytes = 0;
  for (const [key, p] of waiting) {
    const size = utf8Length(JSON.stringify(p.change)) + 1;
    if (batch.length === CHANGES_MAX || (batch.length && bytes + size > BATCH_BYTES)) break;
    batch.push({ key, seq: p.seq, change: p.change });
    bytes += size;
  }
  return batch;
}

/* 1 if two rows hold the same (rev aside). */
function sameRow(a, b) {
  return Boolean(a) && JSON.stringify({ ...a, rev: 0 }) === JSON.stringify({ ...b, rev: 0 });
}

/* Takes a sync's reply in; true if what is shown changed. */
function takeReply(reply, sent) {
  for (const s of sent) {
    const waiting = state.pending[s.key];
    const rev = s.change.kind === "note" ? reply.applied[s.change.id] : undefined;
    if (waiting && waiting.seq === s.seq) {
      delete state.pending[s.key];
      if (rev !== undefined && state.notes[s.change.id]) state.notes[s.change.id].rev = rev;
    } else if (waiting && rev !== undefined) {
      /* Changed again while it was sent: the next change starts from it. */
      waiting.change.base = rev;
    }
  }
  let changed = false;
  if (reply.reset) {
    /* The server's data came back from a backup: drop what it lacks. */
    for (const kind of ["reminder", "note", "tag"]) {
      for (const id of Object.keys(state[kind + "s"])) {
        if (!state.pending[`${kind}:${id}`]) delete state[kind + "s"][id];
      }
    }
    changed = true;
  }
  for (const [kind, rows] of [["reminder", reply.reminders], ["note", reply.notes],
                              ["tag", reply.tags]]) {
    const local = state[kind + "s"];
    for (const { deleted, ...row } of rows) {
      if (state.pending[`${kind}:${row.id}`]) continue; /* this device's change goes first */
      if (deleted) {
        changed = changed || Boolean(local[row.id]);
        delete local[row.id];
      } else {
        changed = changed || !sameRow(local[row.id], row);
        local[row.id] = row;
      }
    }
  }
  state.since = reply.rev;
  return changed;
}

function waitingText() {
  const n = Object.keys(state.pending).length;
  return n ? ` ${plural(n, "change", "changes")} wait on this device.` : "";
}

/*
 * Shows what a sync brought. A field whose text is kept across redraws
 * (data-draft) gets the focus back, where it was; while someone types in
 * any other field, it waits for the next redraw.
 */
async function showChanges() {
  const a = document.activeElement;
  const field = a && app.contains(a) && (a.tagName === "TEXTAREA" || a.tagName === "INPUT");
  if (field && !a.hasAttribute("data-draft") && a.value) {
    staleView = true;
    return;
  }
  staleView = false;
  const label = field ? a.getAttribute("aria-label") : null;
  const caret = field ? [a.selectionStart, a.selectionEnd] : null;
  await refresh();
  const again = label && [...app.querySelectorAll("[data-draft]")]
    .find((n) => n.getAttribute("aria-label") === label);
  if (again) {
    again.focus();
    if (caret[0] !== null) again.setSelectionRange(caret[0], caret[1]);
  }
}

/* Sends what waits and takes in what changed elsewhere, until both are done. */
async function sync() {
  clearTimeout(syncTimer);
  if (syncRun) {
    syncAgain = true;
    return;
  }
  let changed = false;
  syncRun = (async () => {
    for (let round = 0; round < SYNC_ROUNDS; round++) {
      const sent = nextBatch();
      const reply = await api("POST", "/api/notes/sync",
                              { since: state.since, changes: sent.map((s) => s.change) });
      changed = takeReply(reply, sent) || changed;
      if (!reply.more && !Object.keys(state.pending).length) break;
      await saveState();
    }
    state.synced = Date.now();
    await saveState();
  })();
  try {
    await syncRun;
    setStatus(`Synced ${showTime(state.synced / 1000)}.${waitingText()}`);
  } catch (err) {
    if (err instanceof ApiError && err.status === 0) {
      setStatus(`Offline: working on this device.${waitingText()}`);
    } else {
      handleError(err);
    }
  } finally {
    syncRun = null;
  }
  if (changed || staleView) showChanges();
  if (syncAgain) {
    syncAgain = false;
    sync();
  }
}

/* ---- markdown --------------------------------------------------------- */

/* A line's **bold**, *italic*, _italic_ and `code`, as nodes. */
function inline(s) {
  const re = /`([^`]+)`|\*\*(.+?)\*\*|\*([^*\s](?:[^*]*[^*\s])?)\*|(?<!\w)_([^_\s](?:[^_]*[^_\s])?)_(?!\w)/g;
  const out = [];
  let last = 0;
  let m;
  while ((m = re.exec(s))) {
    if (m.index > last) out.push(s.slice(last, m.index));
    if (m[1] !== undefined) out.push(el("code", {}, m[1]));
    else if (m[2] !== undefined) out.push(el("strong", {}, inline(m[2])));
    else out.push(el("em", {}, inline(m[3] !== undefined ? m[3] : m[4])));
    last = re.lastIndex;
  }
  if (last < s.length) out.push(s.slice(last));
  return out;
}

/*
 * A note's text as elements, with a little Markdown: "# ", "## ", "### "
 * headings; "- " or "* " lists, "1. " numbered lists and "- [ ] " tasks
 * (ticking one calls onTask(its line number)); "> " quotes; ``` code
 * blocks ```; and in a line, what inline() knows. Everything else is
 * text as written: lines in a row are one paragraph.
 */
function markdown(text, onTask) {
  const lines = text.split("\n");
  const out = [];
  let block = null; /* {type, node}: the list, quote or paragraph lines join */
  const into = (type, make) => {
    if (!block || block.type !== type) {
      block = { type, node: make() };
      out.push(block.node);
      return true;
    }
    return false;
  };
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    let m;
    if (/^```/.test(line)) {
      const code = [];
      for (i++; i < lines.length && !/^```/.test(lines[i]); i++) code.push(lines[i]);
      out.push(el("pre", {}, el("code", {}, code.join("\n"))));
      block = null;
    } else if ((m = /^(#{1,3}) +(.*)$/.exec(line))) {
      out.push(el(`h${m[1].length + 2}`, { class: "md-h" }, inline(m[2])));
      block = null;
    } else if ((m = /^\s*[-*] \[([ xX])\] ?(.*)$/.exec(line))) {
      into("task", () => el("ul", { class: "tasks" }));
      const done = m[1] !== " ";
      const at = i;
      block.node.append(el("li", { class: done ? "done" : null },
        el("label", { class: "choice" },
          el("input", { type: "checkbox", checked: done, onchange: () => onTask(at) }),
          el("span", {}, inline(m[2])))));
    } else if ((m = /^\s*[-*] (.*)$/.exec(line))) {
      into("ul", () => el("ul"));
      block.node.append(el("li", {}, inline(m[1])));
    } else if ((m = /^\s*\d{1,9}[.)] (.*)$/.exec(line))) {
      into("ol", () => el("ol"));
      block.node.append(el("li", {}, inline(m[1])));
    } else if ((m = /^> ?(.*)$/.exec(line))) {
      if (!into("quote", () => el("blockquote"))) block.node.append(el("br"));
      block.node.append(...inline(m[1]));
    } else if (!line.trim()) {
      block = null;
    } else {
      if (!into("p", () => el("p"))) block.node.append(el("br"));
      block.node.append(...inline(line));
    }
  }
  return out;
}

/* ---- shared parts ----------------------------------------------------- */

const TABS = [
  ["notes", "Notes"],
  ["tags", "Tags"],
  ["stats", "Stats"],
];

function shell(active, ...content) {
  return el("section", { class: "page" },
    el("header", { class: "app-head" },
      el("h1", {}, "Notes"),
      el("nav", { class: "tabs", "aria-label": "Notes sections" },
        TABS.map(([key, label]) =>
          el("a", { href: `#/${key}`, "aria-current": key === active ? "page" : null },
             label)))),
    ...content);
}

function liveTags() {
  return Object.values(state.tags).sort((a, b) => a.name.localeCompare(b.name));
}

function tagChip(tag) {
  return el("span", { class: `chip c-${tag.color}` }, tag.name);
}

/* How many notes have each tag, by tag id. */
function noteCountByTag() {
  const counts = new Map();
  for (const n of Object.values(state.notes)) {
    for (const id of n.tags) counts.set(id, (counts.get(id) || 0) + 1);
  }
  return counts;
}

/*
 * A button per tag that switches on and off; selected (a Set of ids) is
 * changed in place, then onchange() runs. extra: more buttons at the end.
 */
function tagToggles(label, selected, onchange, ...extra) {
  const tags = liveTags();
  if (!tags.length && !extra.length) return null;
  return el("div", { class: "tag-toggles", role: "group", "aria-label": label },
    tags.map((t) => {
      const b = el("button", {
        type: "button", class: `chip toggle c-${t.color}`,
        "aria-pressed": String(selected.has(t.id)),
        onclick: () => {
          if (selected.has(t.id)) selected.delete(t.id);
          else selected.add(t.id);
          b.setAttribute("aria-pressed", String(selected.has(t.id)));
          if (onchange) onchange();
        },
      }, t.name);
      return b;
    }),
    extra);
}

/* Focuses the first element matching selector once the page is drawn. */
function focusLater(selector) {
  setTimeout(() => {
    const node = app.querySelector(selector);
    if (node) node.focus();
  }, 0);
}

/* ---- notes page: reminders -------------------------------------------- */

/* What is being typed, kept when the page is drawn again (a sync may). */
const draft = { reminder: "", note: "", tags: new Set() };
/* The note open for editing: {id, text, tags: Set}, or null. */
let editing = null;
/* Which notes are shown. */
const filter = { text: "", tags: new Set(), untagged: false };

function remindersSection() {
  const reminders = Object.values(state.reminders)
    .sort((a, b) => a.created - b.created || a.id.localeCompare(b.id));
  const input = el("input", { maxlength: REMINDER_MAX, "aria-label": "New reminder",
                              "data-draft": true, placeholder: "Something to do now",
                              value: draft.reminder,
                              oninput: () => { draft.reminder = input.value; } });
  return el("section", { class: "section" },
    el("header", {},
      el("h2", {}, "Reminders ", el("span", { class: "count" }, reminders.length))),
    form({ class: "reminder-add" }, async () => {
      const text = cleanText(input.value, false).trim();
      if (!text) throw invalid("Write the reminder first.");
      if (utf8Length(text) > REMINDER_MAX) {
        throw invalid(`A reminder is at most ${REMINDER_MAX} bytes.`);
      }
      draft.reminder = "";
      await addReminder(text);
      focusLater(".reminder-add input");
    },
      input,
      el("button", { class: "btn go", type: "submit" }, "Add")),
    reminders.length
      ? el("ul", { class: "list reminders" }, reminders.map((r) =>
          el("li", { class: "card reminder" },
            el("button", { class: "btn tick", type: "button", "aria-label": `Done: ${r.text}`,
                           onclick: () => removeReminder(r.id).catch(handleError) }, "✓"),
            el("span", {}, r.text))))
      : el("p", { class: "empty" }, "Nothing to do right now."));
}

/* ---- notes page: notes ------------------------------------------------ */

/* The note's text as the server takes it, or throws why not. */
function checkNote(raw, tags) {
  const text = cleanText(raw, true).trimEnd();
  if (!text.trim()) throw invalid("Write the note first.");
  if (utf8Length(text) > NOTE_MAX) throw invalid(`A note is at most ${NOTE_MAX} bytes.`);
  if (tags.length > NOTE_TAGS_MAX) throw invalid(`A note has at most ${NOTE_TAGS_MAX} tags.`);
  return text;
}

/* The ids in tags (a Set or a list) of tags that still exist. */
function liveIds(tags) {
  return [...tags].filter((id) => state.tags[id]);
}

function composer() {
  const text = el("textarea", { "aria-label": "New note", "data-draft": true,
                                placeholder: "Take a note… # heading, **bold**, *italic*, " +
                                             "`code`, - list, - [ ] task",
                                oninput: () => { draft.note = text.value; } }, draft.note);
  return form({ class: "raised composer" }, async () => {
    const tags = liveIds(draft.tags);
    const body = checkNote(text.value, tags);
    const kept = { ...draft };
    draft.note = "";
    draft.tags = new Set();
    try {
      await saveNote(null, body, tags);
    } catch (err) {
      Object.assign(draft, kept);
      throw err;
    }
  },
    text,
    tagToggles("Tags of the new note", draft.tags),
    el("div", { class: "actions" }, el("button", { class: "btn go", type: "submit" }, "Add note")));
}

/* Ticks or unticks the task on line `line` of a note. */
async function toggleTask(note, line) {
  const lines = note.text.split("\n");
  lines[line] = lines[line].replace(/^(\s*[-*] \[)([ xX])\]/,
                                    (all, start, mark) => `${start}${mark === " " ? "x" : " "}]`);
  try {
    await saveNote(note.id, lines.join("\n"), liveIds(note.tags));
  } catch (err) {
    handleError(err);
  }
}

function noteEditor(note) {
  const text = el("textarea", { "aria-label": "Note", "data-draft": true,
                                oninput: () => { editing.text = text.value; } }, editing.text);
  const close = () => {
    editing = null;
    refresh();
  };
  return el("article", { class: "note-card" }, form({ class: "raised note-editor" }, async () => {
    const tags = liveIds(editing.tags);
    const body = checkNote(text.value, tags);
    if (body === note.text && tags.join() === liveIds(note.tags).join()) {
      close();
      return;
    }
    editing = null;
    await saveNote(note.id, body, tags);
  },
    text,
    tagToggles("Tags of this note", editing.tags),
    el("div", { class: "actions" },
      el("button", { class: "btn go", type: "submit" }, "Save"),
      el("button", { class: "btn", type: "button", onclick: close }, "Cancel"),
      el("button", { class: "btn danger", type: "button", onclick: async () => {
        if (!sure("Delete this note?")) return;
        editing = null;
        try {
          await deleteNote(note.id);
          setStatus("Deleted the note");
        } catch (err) {
          handleError(err);
        }
      } }, "Delete"))));
}

function noteCard(note) {
  if (editing && editing.id === note.id) return noteEditor(note);
  const tags = note.tags.map((id) => state.tags[id]).filter(Boolean);
  const waiting = state.pending[`note:${note.id}`];
  return el("article", { class: "card note-card" },
    el("div", { class: "md" }, markdown(note.text, (line) => toggleTask(note, line))),
    tags.length ? el("div", { class: "tag-list" }, tags.map(tagChip)) : null,
    el("footer", {},
      el("span", { class: "muted small" },
         showTime(note.updated) + (waiting ? " · not synced yet" : "")),
      el("button", { class: "btn", type: "button", onclick: () => {
        editing = { id: note.id, text: note.text, tags: new Set(liveIds(note.tags)) };
        refresh();
        focusLater(".note-editor textarea");
      } }, "Edit")));
}

function matches(note) {
  const tags = liveIds(note.tags);
  if (filter.untagged && tags.length) return false;
  for (const id of filter.tags) if (!tags.includes(id)) return false;
  return !filter.text || note.text.toLowerCase().includes(filter.text.toLowerCase());
}

/* Search and tag buttons; onchange() shows the notes again. */
function filterBar(onchange) {
  for (const id of [...filter.tags]) if (!state.tags[id]) filter.tags.delete(id);
  const search = el("input", { type: "search", "aria-label": "Search the notes",
                               "data-draft": true, placeholder: "Search the notes",
                               value: filter.text,
                               oninput: () => { filter.text = search.value; onchange(); } });
  const chips = el("div");
  /* Untagged shows only notes without tags: it and the tags exclude each other. */
  const drawChips = () => {
    const untagged = el("button", {
      type: "button", class: "chip toggle untagged", "aria-pressed": String(filter.untagged),
      onclick: () => {
        filter.untagged = !filter.untagged;
        if (filter.untagged) filter.tags.clear();
        drawChips();
        onchange();
      },
    }, "Untagged");
    chips.replaceChildren(tagToggles("Show the notes with", filter.tags, () => {
      filter.untagged = false;
      drawChips();
      onchange();
    }, untagged));
  };
  drawChips();
  return el("div", { class: "filters stack" }, search, chips);
}

function notesSection() {
  const notes = Object.values(state.notes)
    .sort((a, b) => b.updated - a.updated || a.id.localeCompare(b.id));
  const count = el("span", { class: "count" });
  const list = el("div", { class: "notes-grid" });
  const show = () => {
    const shown = notes.filter(matches);
    count.textContent = shown.length === notes.length ? String(notes.length)
                                                      : `${shown.length} of ${notes.length}`;
    list.replaceChildren(...(shown.length
      ? shown.map(noteCard)
      : [el("p", { class: "empty" }, notes.length ? "No note matches." : "No notes yet.")]));
  };
  show();
  return el("section", { class: "section" },
    el("header", {}, el("h2", {}, "Notes ", count)),
    composer(),
    notes.length ? filterBar(show) : null,
    list);
}

function notesPage() {
  return shell("notes", remindersSection(), notesSection());
}

/* ---- tags page -------------------------------------------------------- */

/* A tag's name as the server takes it, or throws why not. id: the tag
 * being renamed (null: a new one). */
function checkTagName(raw, id) {
  const name = cleanText(raw, false).trim();
  if (!name) throw invalid("Give the tag a name.");
  if (utf8Length(name) > TAG_MAX) throw invalid(`A tag's name is at most ${TAG_MAX} bytes.`);
  const lower = name.toLowerCase();
  if (liveTags().some((t) => t.id !== id && t.name.toLowerCase() === lower)) {
    throw invalid(`There is a tag named ${name} already.`);
  }
  return name;
}

function tagRow(tag, count) {
  const name = el("input", { value: tag.name, required: true, maxlength: TAG_MAX,
                             "aria-label": "Name" });
  const color = colorPicker(tag.color);
  color.hidden = true;
  const showColors = el("button", { class: "btn", type: "button", onclick: () => {
    color.hidden = false;
    showColors.hidden = true;
  } }, "Change colour");
  return el("li", { class: "card" },
    form({}, async () => {
      const n = checkTagName(name.value, tag.id);
      await saveTag(tag.id, n, color.value);
      setStatus(`Saved ${n}`);
    },
      el("div", { class: "item" }, tagChip(tag),
         el("span", { class: "muted" }, plural(count, "note", "notes"))),
      name,
      color,
      el("div", { class: "actions" },
        el("button", { class: "btn", type: "submit" }, "Save"),
        showColors,
        count ? el("button", { class: "btn", type: "button", onclick: () => {
          filter.tags = new Set([tag.id]);
          filter.untagged = false;
          filter.text = "";
          go("#/notes");
        } }, "Show notes") : null,
        el("button", { class: "btn danger", type: "button", onclick: async () => {
          if (!sure(`Delete the tag ${tag.name}?` +
                    (count ? ` ${plural(count, "note loses", "notes lose")} it; ` +
                             "their text stays." : ""))) return;
          try {
            await deleteTag(tag.id);
            setStatus(`Deleted ${tag.name}`);
          } catch (err) {
            handleError(err);
          }
        } }, "Delete"))));
}

function tagsPage() {
  const tags = liveTags();
  const counts = noteCountByTag();
  const name = el("input", { required: true, maxlength: TAG_MAX, "aria-label": "New tag",
                             placeholder: "e.g. Shopping" });
  const color = colorPicker(nextColor(tags.map((t) => t.color)));
  return shell("tags",
    el("section", { class: "section" },
      el("p", { class: "hint" },
         "Tags sort the notes: give notes tags, then show only the notes with a tag."),
      form({ class: "raised" }, async () => {
        const n = checkTagName(name.value, null);
        await saveTag(null, n, color.value);
        setStatus(`Added ${n}`);
      },
        name,
        color,
        el("div", { class: "actions" },
          el("button", { class: "btn go", type: "submit" }, "Add tag")))),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Tags ", el("span", { class: "count" }, tags.length))),
      tags.length
        ? el("ul", { class: "list cols" }, tags.map((t) => tagRow(t, counts.get(t.id) || 0)))
        : el("p", { class: "empty" }, "No tags yet.")));
}

/* ---- stats page ------------------------------------------------------- */

function stat(label, value) {
  return el("div", {}, el("dt", {}, label), el("dd", {}, String(value)));
}

/* A labelled bar: n of max. */
function bar(label, n, max) {
  return el("li", { class: "count-bar" },
    el("span", {}, label),
    el("meter", { min: 0, max, value: n, "aria-hidden": "true" }),
    el("span", { class: "num" }, String(n)));
}

/* Notes added in each of the last 12 months, oldest first: [{label, n}]. */
function notesPerMonth(notes) {
  const now = new Date();
  const months = [];
  for (let i = 11; i >= 0; i--) {
    const d = new Date(now.getFullYear(), now.getMonth() - i, 1);
    months.push({ y: d.getFullYear(), m: d.getMonth(), n: 0,
                  label: `${MONTHS[d.getMonth()]} ${d.getFullYear()}` });
  }
  for (const note of notes) {
    const d = new Date(note.created * 1000);
    const month = months.find((x) => x.y === d.getFullYear() && x.m === d.getMonth());
    if (month) month.n++;
  }
  return months;
}

function statsPage() {
  const notes = Object.values(state.notes);
  const reminders = Object.values(state.reminders);
  const tags = liveTags();
  const counts = noteCountByTag();
  const now = nowSeconds();
  const words = notes.reduce((n, x) => n + (x.text.match(/\S+/g) || []).length, 0);
  const since = (days) => notes.filter((n) => now - n.created < days * 86400).length;
  const untagged = notes.filter((n) => !liveIds(n.tags).length).length;
  const oldest = reminders.reduce((a, r) => (!a || r.created < a.created ? r : a), null);
  const byTag = tags.map((t) => ({ tag: t, n: counts.get(t.id) || 0 })).sort((a, b) => b.n - a.n);
  const tagMax = Math.max(1, ...byTag.map((x) => x.n));
  const months = notesPerMonth(notes);
  const monthMax = Math.max(1, ...months.map((x) => x.n));

  return shell("stats",
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "At a glance")),
      el("dl", { class: "stats" },
        stat("Notes", notes.length),
        stat("Words in them", words),
        stat("Added in 7 days", since(7)),
        stat("Added in 30 days", since(30)),
        stat("Without tags", untagged),
        stat("Tags", tags.length),
        stat("Reminders to do", reminders.length),
        stat("Oldest reminder", oldest ? showTime(oldest.created) : "—"))),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Notes by tag")),
      byTag.length
        ? el("ul", { class: "count-bars" }, byTag.map((x) => bar(tagChip(x.tag), x.n, tagMax)))
        : el("p", { class: "empty" }, "No tags yet.")),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "Notes added per month")),
      el("ul", { class: "count-bars" }, months.map((x) => bar(x.label, x.n, monthMax)))),
    el("section", { class: "section" },
      el("header", {}, el("h2", {}, "This device")),
      el("dl", { class: "stats" },
        stat("Last synced", state.synced ? showTime(state.synced / 1000) : "Never"),
        stat("Changes waiting", Object.keys(state.pending).length))));
}

/* ---- routing ---------------------------------------------------------- */

/* ["tags"] -> the tags page, etc. Unknown hashes go to Notes. Each page
 * shown syncs (soon, to gather quick changes into one request). */
function route(parts) {
  const [section, extra] = parts;
  const page = extra !== undefined ? null
             : section === "notes" ? notesPage
             : section === "tags" ? tagsPage
             : section === "stats" ? statsPage
             : null;
  if (!page) {
    go("#/notes");
    return null;
  }
  if (firstSync) {
    firstSync = false;
    sync();
  } else {
    scheduleSync();
  }
  return page();
}

async function start() {
  try {
    await loadState();
  } catch (err) {
    setStatus(err.message, true);
    return;
  }
  window.addEventListener("online", () => sync());
  document.addEventListener("visibilitychange", async () => {
    if (document.visibilityState !== "visible" || syncRun) return;
    try {
      await loadState(); /* another tab may have changed it */
    } catch (err) {
      handleError(err);
      return;
    }
    staleView = true;
    sync();
  });
  startApp(route);
}

start();
