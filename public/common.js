"use strict";

/*
 * Shared by every nylm page: helpers, forms, the session (login / logout),
 * the hash router, and the UI parts every app uses: dropdown, calendar,
 * colour picker.
 *
 * Each app is a page (public/<app>/index.html) that loads this file and then
 * its own script, which calls startApp(route). The server only serves files
 * and JSON; everything the user sees is built in JavaScript. Rule: data goes
 * into the page with textContent (via el()), never innerHTML.
 */

const app = document.getElementById("app");
const statusLine = document.getElementById("status");
const logoutButton = document.getElementById("logout");
const homeButton = document.getElementById("home");

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

/* A labelled form control. Custom controls (dropdown, calendar) are not
 * wrapped in <label>: a click on the label would also click their button. */
function field(label, control, hint) {
  const native = ["input", "textarea"].includes(control.tagName.toLowerCase());
  return el(native ? "label" : "div", { class: "field" }, el("span", {}, label), control,
            hint ? el("span", { class: "hint" }, hint) : null);
}

/*
 * A button that goes to another page. Everything that navigates looks like
 * a button; text is never a link.
 */
function navButton(label, href, extra) {
  return el("a", { class: extra ? "btn " + extra : "btn", href }, label);
}

/*
 * A form whose submit runs action(form). While it runs the form's buttons
 * are disabled; an error is shown in the form's .form-error line.
 */
function form(attrs, action, ...children) {
  const error = el("p", { class: "form-error", role: "alert" });
  const node = el("form", {
    ...attrs,
    class: attrs.class ? "form " + attrs.class : "form",
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

function plural(n, one, many) {
  return `${n} ${n === 1 ? one : many}`;
}

/* A page-unique id for aria references. */
let idSeq = 0;
function uniqueId(prefix) {
  return `${prefix}-${++idSeq}`;
}

/* ---- dates ------------------------------------------------------------- */

const MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
const MONTH_NAMES = ["January", "February", "March", "April", "May", "June", "July",
                     "August", "September", "October", "November", "December"];
const DAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const DAY_NAMES = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                   "Saturday"];

function pad2(n) {
  return String(n).padStart(2, "0");
}

function isoDate(y, m, d) {
  return `${y}-${pad2(m)}-${pad2(d)}`;
}

/* "YYYY-MM-DD" -> {y, m, d} */
function parseDate(iso) {
  const [y, m, d] = iso.split("-").map(Number);
  return { y, m, d };
}

/* Today as "YYYY-MM-DD" in local time. */
function today() {
  const d = new Date();
  return isoDate(d.getFullYear(), d.getMonth() + 1, d.getDate());
}

/* "2026-10-04" -> "Sun 4 Oct" (this year) or "4 Oct 2027". */
function showDate(iso) {
  const { y, m, d } = parseDate(iso);
  const date = new Date(y, m - 1, d);
  if (y === new Date().getFullYear()) return `${DAYS[date.getDay()]} ${d} ${MONTHS[m - 1]}`;
  return `${d} ${MONTHS[m - 1]} ${y}`;
}

/* "3 Mar" for a month/day pair. */
function showMonthDay(month, day) {
  return `${day} ${MONTHS[month - 1]}`;
}

/* Days in a month (1-12); 2000 is a leap year, used when there is no year. */
function daysInMonth(y, m) {
  return new Date(y, m, 0).getDate();
}

/* ---- popovers ---------------------------------------------------------- */

/*
 * At most one dropdown or calendar is open. A press outside it or Escape
 * closes it; Escape also returns focus to the control that opened it.
 */
let openPopover = null; /* { owner, close(refocus) } */

function popoverOpened(owner, pop, close) {
  if (openPopover && openPopover.owner !== owner) openPopover.close(false);
  openPopover = { owner, close };
  /* Keep it on screen: open towards the left if it would overflow. */
  pop.classList.remove("align-right");
  if (pop.getBoundingClientRect && pop.getBoundingClientRect().right > window.innerWidth - 8) {
    pop.classList.add("align-right");
  }
}

function popoverClosed(owner) {
  if (openPopover && openPopover.owner === owner) openPopover = null;
}

document.addEventListener("pointerdown", (e) => {
  if (openPopover && !openPopover.owner.contains(e.target)) openPopover.close(false);
});
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && openPopover) {
    e.preventDefault();
    openPopover.close(true);
  }
});

/* ---- dropdown ---------------------------------------------------------- */

/*
 * A dropdown in place of <select>. options: [{value, label, render?}], where
 * render() returns what to show (e.g. a coloured chip) instead of label.
 * The returned element has .value and fires a bubbling "change" event;
 * onchange(value) is called too.
 *
 * Keys: Enter / Space / arrows open it; arrows, Home, End move; Enter or
 * Space chooses; a letter jumps to the next option starting with it.
 */
function dropdown({ label, options, value, onchange }) {
  let index = Math.max(0, options.findIndex((o) => o.value === value));
  let active = index;
  const show = (o) => (o.render ? o.render() : o.label);

  const current = el("span", { class: "picker-value" });
  const button = el("button", { type: "button", class: "picker-btn select",
                                "aria-haspopup": "listbox", "aria-expanded": "false" }, current);
  const listId = uniqueId("list");
  const list = el("ul", { class: "popover listbox", role: "listbox", id: listId, tabindex: "-1",
                          "aria-label": label, hidden: true });
  const items = options.map((o, i) => el("li", {
    role: "option", id: `${listId}-${i}`, class: "option",
    onclick: () => choose(i),
    onpointermove: () => highlight(i),
  }, show(o)));
  list.append(...items);
  const wrap = el("div", { class: "picker" }, button, list);

  function render() {
    current.replaceChildren(show(options[index]));
    button.setAttribute("aria-label", `${label}: ${options[index].label}`);
    items.forEach((li, i) => li.setAttribute("aria-selected", String(i === index)));
  }
  function highlight(i) {
    active = i;
    items.forEach((li, j) => li.classList.toggle("active", j === i));
    list.setAttribute("aria-activedescendant", items[i].id);
    if (items[i].scrollIntoView) items[i].scrollIntoView({ block: "nearest" });
  }
  function open() {
    list.hidden = false;
    button.setAttribute("aria-expanded", "true");
    popoverOpened(wrap, list, close);
    highlight(index);
    list.focus();
  }
  function close(refocus) {
    list.hidden = true;
    button.setAttribute("aria-expanded", "false");
    popoverClosed(wrap);
    if (refocus) button.focus();
  }
  function choose(i) {
    const changed = i !== index;
    index = i;
    render();
    close(true);
    if (changed) {
      if (onchange) onchange(options[i].value);
      wrap.dispatchEvent(new Event("change", { bubbles: true }));
    }
  }

  button.addEventListener("click", () => (list.hidden ? open() : close(false)));
  button.addEventListener("keydown", (e) => {
    if (["ArrowDown", "ArrowUp", "Enter", " "].includes(e.key)) {
      e.preventDefault();
      open();
    }
  });
  list.addEventListener("keydown", (e) => {
    const last = options.length - 1;
    if (e.key === "ArrowDown") highlight(Math.min(active + 1, last));
    else if (e.key === "ArrowUp") highlight(Math.max(active - 1, 0));
    else if (e.key === "Home") highlight(0);
    else if (e.key === "End") highlight(last);
    else if (e.key === "Enter" || e.key === " ") choose(active);
    else if (e.key === "Tab") { close(false); return; }
    else if (e.key.length === 1) {
      const letter = e.key.toLowerCase();
      for (let step = 1; step <= options.length; step++) {
        const i = (active + step) % options.length;
        if (options[i].label.toLowerCase().startsWith(letter)) {
          highlight(i);
          break;
        }
      }
    } else return;
    e.preventDefault();
  });

  Object.defineProperty(wrap, "value", { get: () => options[index].value });
  render();
  return wrap;
}

/* ---- calendar ---------------------------------------------------------- */

const WEEKDAYS = ["Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"];

/*
 * A month grid. start {y, m, d} is the day that has focus first; selected is
 * the chosen day (or null). Without a year (yearless) the months cycle and
 * there is no weekday row. isDisabled(y, m, d) greys days out (they can
 * still be focused, not picked). onPick(y, m, d). footer: extra buttons.
 *
 * Keys on a day: arrows move a day / a week, PageUp / PageDown a month,
 * Home / End the first / last day, Enter or Space picks.
 */
function calendar({ yearless, start, selected, isDisabled, onPick, footer }) {
  let active = { ...start };
  const root = el("div", { class: "calendar" });
  const now = parseDate(today());

  function moveDays(n) {
    const t = new Date(active.y, active.m - 1, active.d + n);
    active = { y: yearless ? 2000 : t.getFullYear(), m: t.getMonth() + 1, d: t.getDate() };
    render(true);
  }
  function moveMonths(n) {
    const index = active.m - 1 + n;
    const y = yearless ? 2000 : active.y + Math.floor(index / 12);
    const m = ((index % 12) + 12) % 12 + 1;
    active = { y, m, d: Math.min(active.d, daysInMonth(y, m)) };
    render(true);
  }

  function dayButton(d) {
    const { y, m } = active;
    const off = Boolean(isDisabled && isDisabled(y, m, d));
    const chosen = Boolean(selected && selected.m === m && selected.d === d &&
                           (yearless || selected.y === y));
    const isToday = !yearless && now.y === y && now.m === m && now.d === d;
    const weekday = yearless ? "" : DAY_NAMES[new Date(y, m - 1, d).getDay()] + " ";
    const classes = ["cal-day", chosen ? "selected" : "", isToday ? "today" : ""];
    return el("button", {
      type: "button",
      class: classes.filter(Boolean).join(" "),
      tabindex: d === active.d ? "0" : "-1",
      "aria-pressed": String(chosen),
      "aria-disabled": off ? "true" : null,
      "aria-label": `${weekday}${d} ${MONTH_NAMES[m - 1]}${yearless ? "" : " " + y}` +
                    (isToday ? ", today" : ""),
      onclick: () => { if (!off) onPick(y, m, d); },
    }, String(d));
  }

  function render(focus) {
    const { y, m } = active;
    const blanks = yearless ? 0 : (new Date(y, m - 1, 1).getDay() + 6) % 7; /* Monday first */
    const days = [];
    for (let i = 0; i < blanks; i++) days.push(el("span", { class: "cal-blank" }));
    for (let d = 1; d <= daysInMonth(y, m); d++) days.push(dayButton(d));
    /* replaceChildren would turn a null into the text "null": filter them. */
    root.replaceChildren(...[
      el("div", { class: "cal-head" },
        el("button", { type: "button", class: "btn cal-nav", "aria-label": "Previous month",
                       onclick: () => moveMonths(-1) }, "‹"),
        el("strong", { "aria-live": "polite" },
           yearless ? MONTH_NAMES[m - 1] : `${MONTH_NAMES[m - 1]} ${y}`),
        el("button", { type: "button", class: "btn cal-nav", "aria-label": "Next month",
                       onclick: () => moveMonths(1) }, "›")),
      yearless ? null
               : el("div", { class: "cal-week", "aria-hidden": "true" },
                    WEEKDAYS.map((w) => el("span", {}, w))),
      el("div", { class: "cal-grid" }, days),
      footer,
    ].filter(Boolean));
    if (focus) root.focusDay();
  }

  root.focusDay = () => {
    const b = root.querySelector(".cal-day[tabindex=\"0\"]");
    if (b) b.focus();
  };
  root.addEventListener("keydown", (e) => {
    if (!e.target.classList || !e.target.classList.contains("cal-day")) return;
    const moves = { ArrowLeft: -1, ArrowRight: 1, ArrowUp: -7, ArrowDown: 7 };
    if (e.key in moves) moveDays(moves[e.key]);
    else if (e.key === "PageUp") moveMonths(-1);
    else if (e.key === "PageDown") moveMonths(1);
    else if (e.key === "Home") moveDays(1 - active.d);
    else if (e.key === "End") moveDays(daysInMonth(active.y, active.m) - active.d);
    else return;
    e.preventDefault();
  });
  render(false);
  return root;
}

/*
 * The button + popover around a calendar. text() is what the button shows;
 * build(close) makes the calendar each time it opens.
 */
function calendarPicker(label, text, build) {
  const shown = el("span", { class: "picker-value" });
  const button = el("button", { type: "button", class: "picker-btn date",
                                "aria-haspopup": "dialog", "aria-expanded": "false" }, shown);
  const pop = el("div", { class: "popover", role: "dialog", "aria-label": label, hidden: true });
  const wrap = el("div", { class: "picker" }, button, pop);

  function close(refocus) {
    pop.hidden = true;
    button.setAttribute("aria-expanded", "false");
    popoverClosed(wrap);
    if (refocus) button.focus();
  }
  function open() {
    const cal = build(close);
    pop.replaceChildren(cal);
    pop.hidden = false;
    button.setAttribute("aria-expanded", "true");
    popoverOpened(wrap, pop, close);
    cal.focusDay();
  }
  button.addEventListener("click", () => (pop.hidden ? open() : close(false)));
  wrap.refresh = () => {
    const t = text();
    shown.textContent = t;
    button.setAttribute("aria-label", `${label}: ${t}`);
  };
  wrap.changed = () => {
    wrap.refresh();
    wrap.dispatchEvent(new Event("change", { bubbles: true }));
  };
  return wrap;
}

/*
 * A date ("YYYY-MM-DD") picked on a calendar. max: latest allowed date.
 * nullable: offers "Clear" and allows no date (value null).
 */
function datePicker({ label, value, max, nullable }) {
  let current = value || null;
  const picker = calendarPicker(label, () => (current ? showDate(current) : "Not set"), (close) => {
    const pick = (iso) => {
      current = iso;
      close(true);
      picker.changed();
    };
    const t = today();
    const startIso = current || (max && t > max ? max : t);
    const footer = el("div", { class: "cal-foot" },
      !max || t <= max
        ? el("button", { type: "button", class: "btn", onclick: () => pick(t) }, "Today")
        : null,
      nullable
        ? el("button", { type: "button", class: "btn", onclick: () => pick(null) }, "Clear")
        : null);
    return calendar({
      start: parseDate(startIso),
      selected: current ? parseDate(current) : null,
      isDisabled: (y, m, d) => Boolean(max) && isoDate(y, m, d) > max,
      onPick: (y, m, d) => pick(isoDate(y, m, d)),
      footer,
    });
  });
  Object.defineProperty(picker, "value", { get: () => current });
  picker.refresh();
  return picker;
}

/* A day of the year with no year ({month, day}; 29 Feb allowed). */
function monthDayPicker({ label, month, day }) {
  let current = { month, day };
  const picker = calendarPicker(label, () => showMonthDay(current.month, current.day),
    (close) => calendar({
      yearless: true,
      start: { y: 2000, m: current.month, d: current.day },
      selected: { y: 2000, m: current.month, d: current.day },
      onPick: (y, m, d) => {
        current = { month: m, day: d };
        close(true);
        picker.changed();
      },
    }));
  Object.defineProperty(picker, "value", { get: () => ({ ...current }) });
  picker.refresh();
  return picker;
}

/* ---- colours ----------------------------------------------------------- */

/* The palette for things people name (plants, care types, ...). Must match
 * CARE_COLOR_NAMES in src/care.h. Rose and peach are kept for urgency. */
const COLORS = ["butter", "lime", "mint", "teal", "sky", "periwinkle", "lavender", "orchid"];

/* The first palette colour not in used (or the least used one). */
function nextColor(used) {
  const counts = new Map(COLORS.map((c) => [c, 0]));
  for (const c of used) if (counts.has(c)) counts.set(c, counts.get(c) + 1);
  return COLORS.reduce((best, c) => (counts.get(c) < counts.get(best) ? c : best));
}

/* Round colour swatches to pick from; .value is the chosen name. */
function colorPicker(selected) {
  const name = uniqueId("color");
  const node = el("fieldset", { class: "swatches" },
    el("legend", { class: "hint" }, "Colour"),
    COLORS.map((c) => el("label", { class: `swatch c-${c}` },
      el("input", { type: "radio", name, value: c, checked: c === selected, "aria-label": c }))));
  Object.defineProperty(node, "value", {
    get: () => node.querySelector(`input[name=${name}]:checked`).value,
  });
  return node;
}

/* ---- pages and session ------------------------------------------------- */

/* The page's router: (parts of the hash) -> Promise of the page, or null if
 * it navigated elsewhere. Set by startApp(). */
let pageRoute = null;

/* Each render bumps this; a render that finds it changed was overtaken. */
let renderSeq = 0;

/* "#/journal/3" -> ["journal", "3"] */
function hashParts() {
  return location.hash.replace(/^#\/?/, "").split("/").filter(Boolean);
}

function go(hash) {
  if (location.hash === hash) refresh();
  else location.hash = hash;
}

/* Builds the current page again, e.g. after a change was saved. */
async function refresh() {
  const seq = ++renderSeq;
  logoutButton.hidden = false;
  if (openPopover) popoverClosed(openPopover.owner);
  try {
    const page = await pageRoute(hashParts());
    if (page && seq === renderSeq) app.replaceChildren(page);
  } catch (err) {
    if (seq === renderSeq) handleError(err);
  }
}

function renderLogin() {
  renderSeq++; /* drop any page still loading */
  logoutButton.hidden = true;
  const password = el("input", { type: "password", name: "password", required: true,
                                 autocomplete: "current-password" });
  app.replaceChildren(form({ class: "raised login" }, async () => {
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
    refresh();
  },
    el("h1", {}, "nylm"),
    field("Password", password),
    el("div", { class: "actions" }, el("button", { class: "btn go", type: "submit" }, "Log in"))));
  password.focus();
}

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

/*
 * Starts a page: route(parts) builds it from the URL hash. home: this is the
 * home page (no Home button).
 */
async function startApp(route, { home } = {}) {
  pageRoute = route;
  homeButton.hidden = Boolean(home);
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
  window.addEventListener("hashchange", refresh);
  try {
    await api("GET", "/api/session");
    refresh();
  } catch (err) {
    handleError(err);
  }
}
