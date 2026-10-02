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

/* ---- home ------------------------------------------------------------- */

function renderHome() {
  logoutButton.hidden = false;
  app.replaceChildren(el("section", { class: "panel" }, "Logged in. No tools yet."));
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
    setStatus("logged out");
  } catch (err) {
    setStatus("logout failed: " + err.message, true);
    return;
  }
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
    await api("GET", "/api/session");
    renderHome();
  } catch (err) {
    handleError(err);
  }
}

start();
