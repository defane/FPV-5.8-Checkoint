// Host test for the pages' info bubbles (firmware/node and firmware/master www): both pages carry the same CSS and the
// same script snippet, every "i" button targets a popover note, and the snippet, run against a small DOM stub, places a
// note under its button (above when there is no room, inside the window) and, without the Popover API, opens and closes
// it on a click, Escape and a click elsewhere. Run: node test_web_bubble.mjs
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

let fails = 0, oks = 0;
const check = (name, cond) => { if (cond) oks++; else { console.log("FAIL: " + name); fails++; } };

const file = rel => readFileSync(fileURLToPath(new URL("../" + rel, import.meta.url)), "utf8");
const pages = { node: file("firmware/node/main/www/index.html"), master: file("firmware/master/main/www/index.html") };
const scripts = { node: file("firmware/node/main/www/app.js"), master: file("firmware/master/main/www/app.js") };

// The snippet: from its marker comment to the end of its last rule (CSS) or statement (JS).
function cut(src, start, end) {
  const i = src.indexOf(start), j = i < 0 ? -1 : src.indexOf(end, i);
  return j < 0 ? "" : src.slice(i, j + end.length);
}
const css = src => cut(src, "/* Info bubbles (the same snippet", ".note p { margin: 0 0 6px; } .note p:last-child { margin: 0; }\n");
const js = src => cut(src, "// Info bubbles (the same snippet", 'window.addEventListener("resize", infoClose);\n');
for (const name of Object.keys(pages)) {
  check(`${name}: has the CSS snippet`, css(pages[name]).includes(".note::before"));
  check(`${name}: has the script snippet`, js(scripts[name]).includes("function notePlace"));
}
check("the CSS snippet is the same on both pages", css(pages.node) === css(pages.master));
check("the script snippet is the same on both pages", js(scripts.node) === js(scripts.master));

// Every "i" button names a popover note, nothing is hidden by hand, and the re-arm note says the same on both pages.
for (const [name, src] of Object.entries(pages)) {
  const buttons = [...src.matchAll(/<button type="button" class="info"[^>]*popovertarget="(\w+)"/g)].map(m => m[1]);
  check(`${name}: has info buttons`, buttons.length >= 2);
  check(`${name}: each button's note is a popover`, buttons.every(id => new RegExp(`<div id="${id}" class="note" popover>`).test(src)));
  check(`${name}: no note is hidden by hand, no aria-controls left`, !/class="note"[^>]*hidden/.test(src) && !src.includes("aria-controls"));
}
const rearmNote = src => cut(src, '<div id="rearm_note" class="note" popover>', "</div>");
check("the re-arm note is on both pages", rearmNote(pages.node).includes("Recommended: 16 dB"));
check("the re-arm note says the same on both pages", rearmNote(pages.node) === rearmNote(pages.master));

// The snippet in a stub DOM. popover: whether the browser has the Popover API.
function load(popover) {
  const listeners = { doc: {}, win: {} };
  const cls = () => { const s = new Set(); return { add: c => s.add(c), remove: c => s.delete(c), contains: c => s.has(c),
                                                    toggle: (c, on) => (on ? s.add(c) : s.delete(c)), _s: s }; };
  const node = (id, extra = {}) => ({ id, classList: cls(), dataset: {}, style: { props: {}, setProperty(k, v) { this.props[k] = v; } },
                                       rect: { width: 300, height: 100 }, open: false,
                                       getBoundingClientRect() { return this.rect; },
                                       matches(sel) { return sel === ":popover-open" && this.open; },
                                       hidePopover() { this.open = false; ctx._toggled(this, "closed"); }, ...extra });
  const note = node("a_note"); note.classList.add("note");
  const button = { classList: cls(), rect: { left: 100, width: 24, top: 200, bottom: 224 }, getBoundingClientRect() { return this.rect; },
                   closest(sel) { return sel === "button.info" ? this : null; }, getAttribute: () => "a_note" };
  note.closest = sel => (sel === ".note" ? note : null);
  const documentElement = { classList: cls(), clientWidth: 400, clientHeight: 800 };
  const ctx = vm.createContext({
    HTMLElement: class { }, window: { addEventListener: (t, f) => (listeners.win[t] = f) },
    document: { documentElement,
      addEventListener: (t, f, cap) => (listeners.doc[t] = { f, cap }),
      querySelector: () => button, querySelectorAll: () => [note], getElementById: () => note },
    Math, Set });
  if (popover) ctx.HTMLElement.prototype.popover = null;
  ctx._toggled = (n, state) => listeners.doc.toggle && listeners.doc.toggle.f({ target: n, newState: state });
  vm.runInContext(js(scripts.node) + "\nthis.notePlace = notePlace; this.infoClose = infoClose;", ctx);
  return { ctx, listeners, note, button, documentElement };
}

// Placement.
{
  const { ctx } = load(true);
  const r = { left: 100, width: 24, top: 200, bottom: 224 };
  const under = ctx.notePlace(r, 300, 100, 400, 800);
  check("a note goes under its button, centred on it", under.top === 232 && !under.above && under.left === 8 && under.arrow === 104);
  const right = ctx.notePlace({ left: 380, width: 24, top: 200, bottom: 224 }, 300, 100, 400, 800);
  check("a note stays inside the window on the right", right.left === 92 && right.left + 300 <= 392);
  const left = ctx.notePlace({ left: 0, width: 24, top: 200, bottom: 224 }, 300, 100, 400, 800);
  check("... and on the left", left.left === 8 && left.arrow === 14, JSON.stringify(left));
  const above = ctx.notePlace({ left: 100, width: 24, top: 700, bottom: 724 }, 300, 100, 400, 800);
  check("a note flips above when it does not fit below", above.above && above.top === 700 - 8 - 100);
  const nowhere = ctx.notePlace({ left: 100, width: 24, top: 40, bottom: 64 }, 300, 790, 400, 800);
  check("a note that fits nowhere stays below", !nowhere.above && nowhere.top === 72);
  const wide = ctx.notePlace(r, 500, 100, 400, 800);
  check("a note wider than the window starts at its edge", wide.left === 8 && wide.arrow >= 14);
}

// With the Popover API: the browser opens and closes it; the script places the note when it opens.
{
  const { listeners, note, button, documentElement } = load(true);
  check("popover: no fallback class", !documentElement.classList.contains("no-popover"));
  check("popover: toggle is caught in the capture phase", listeners.doc.toggle && listeners.doc.toggle.cap === true);
  listeners.doc.toggle.f({ target: note, newState: "open" });
  check("popover: opening places the note and shows it", note.style.left === "8px" && note.style.top === "232px" && "placed" in note.dataset);
  check("popover: the arrow is set and the button marked", note.style.props["--arrow"] === "104px" && button.classList.contains("on"));
  listeners.doc.toggle.f({ target: note, newState: "closed" });
  check("popover: closing hides it and unmarks the button", !("placed" in note.dataset) && !button.classList.contains("on"));
  note.open = true;
  listeners.doc.focusin.f({ target: { closest: () => null } });
  check("popover: the focus going elsewhere closes it", note.open === false);
  note.open = true;
  listeners.doc.focusin.f({ target: { closest: sel => (sel === ".note" ? note : null) } });
  check("popover: the focus inside the note keeps it", note.open === true);
  listeners.win.scroll({ target: { closest: () => null } });
  check("popover: a scroll closes it", note.open === false);
}

// Without it: a click opens, a second one closes, Escape and a click elsewhere close.
{
  const { listeners, note, button, documentElement } = load(false);
  check("fallback: the page says so", documentElement.classList.contains("no-popover"));
  listeners.doc.click.f({ target: button });
  check("fallback: a click opens and places the note", note.classList.contains("open") && "placed" in note.dataset && button.classList.contains("on"));
  listeners.doc.click.f({ target: button });
  check("fallback: a second click closes it", !note.classList.contains("open") && !("placed" in note.dataset));
  listeners.doc.click.f({ target: button });
  listeners.doc.keydown.f({ key: "Escape" });
  check("fallback: Escape closes it", !note.classList.contains("open"));
  listeners.doc.click.f({ target: button });
  listeners.doc.click.f({ target: { closest: () => null } });
  check("fallback: a click elsewhere closes it", !note.classList.contains("open"));
}

if (fails) { console.log(`${fails} FAILED, ${oks} ok`); process.exit(1); }
console.log(`all ${oks} checks ok`);
