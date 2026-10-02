"use strict";

/*
 * nylm frontend. The server only serves files and JSON; everything the user
 * sees is built here. Rule: data goes into the page with textContent (via
 * el()), never innerHTML.
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
  for (const child of children) {
    if (child == null) continue;
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

function formatTime(unixSeconds) {
  return new Date(unixSeconds * 1000).toLocaleString();
}

/* ---- notes (example feature) ------------------------------------------ */

const notes = {
  items: [],
  selected: null, /* id of the note in the editor, or null for a new one */
};

async function loadNotes() {
  notes.items = await api("GET", "/api/notes");
}

function renderNotes() {
  const current = notes.items.find((n) => n.id === notes.selected);

  const list = el("ul", { class: "list" },
    ...notes.items.map((n) =>
      el("li", {},
        el("button", {
          "aria-current": n.id === notes.selected ? "true" : "false",
          title: n.title,
          onclick: () => { notes.selected = n.id; renderNotes(); },
        }, n.title))));

  const title = el("input", { name: "title", maxlength: "200", required: true,
                              value: current ? current.title : "" });
  const body = el("textarea", { name: "body", maxlength: "10000" });
  body.value = current ? current.body : "";

  const form = el("form", { onsubmit: (e) => { e.preventDefault(); saveNote(title.value, body.value); } },
    el("label", {}, el("span", {}, "title"), title),
    el("label", {}, el("span", {}, "body"), body),
    el("div", { class: "actions" },
      el("button", { class: "btn primary", type: "submit" }, current ? "save" : "create"),
      el("button", { class: "btn", type: "button",
                     onclick: () => { notes.selected = null; renderNotes(); } }, "new"),
      current && el("button", { class: "btn danger", type: "button",
                                onclick: () => deleteNote(current.id) }, "delete")),
    current && el("p", { class: "empty" }, "updated " + formatTime(current.updated_at)));

  app.replaceChildren(
    el("div", { class: "split" },
      el("section", { class: "panel" },
        notes.items.length ? list : el("p", { class: "empty" }, "no notes yet")),
      el("section", { class: "panel" }, form)));
}

async function saveNote(title, body) {
  try {
    const saved = notes.selected === null
      ? await api("POST", "/api/notes", { title, body })
      : await api("PUT", "/api/notes/" + notes.selected, { title, body });
    notes.selected = saved.id;
    await loadNotes();
    renderNotes();
    setStatus("saved");
  } catch (err) {
    handleError(err);
  }
}

async function deleteNote(id) {
  if (!confirm("Delete this note?")) return;
  try {
    await api("DELETE", "/api/notes/" + id);
    notes.selected = null;
    await loadNotes();
    renderNotes();
    setStatus("deleted");
  } catch (err) {
    handleError(err);
  }
}

/* ---- session ---------------------------------------------------------- */

function renderLogin() {
  logoutButton.hidden = true;
  const password = el("input", { type: "password", name: "password", required: true,
                                 autocomplete: "current-password" });
  const submit = el("button", { class: "btn primary", type: "submit" }, "log in");

  app.replaceChildren(
    el("form", {
      class: "panel login",
      onsubmit: async (e) => {
        e.preventDefault();
        submit.disabled = true;
        setStatus("checking…");
        try {
          await api("POST", "/api/login", { password: password.value });
          setStatus("");
          await start();
        } catch (err) {
          password.value = "";
          handleError(err);
        } finally {
          submit.disabled = false;
          password.focus();
        }
      },
    },
      el("label", {}, el("span", {}, "password"), password),
      el("div", { class: "actions" }, submit)));
  password.focus();
}

logoutButton.addEventListener("click", async () => {
  try {
    await api("POST", "/api/logout");
  } catch (err) {
    /* logged out either way */
  }
  setStatus("logged out");
  renderLogin();
});

/* ---- startup ---------------------------------------------------------- */

/* Any 401 means the session is gone: show the login form. */
function handleError(err) {
  if (err instanceof ApiError && err.status === 401 && !app.querySelector(".login")) {
    setStatus("please log in");
    renderLogin();
    return;
  }
  setStatus(err.message || String(err), true);
}

async function start() {
  try {
    await loadNotes();
    logoutButton.hidden = false;
    renderNotes();
  } catch (err) {
    handleError(err);
  }
}

start();
