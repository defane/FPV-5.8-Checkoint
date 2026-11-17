// Host test for the pages' info bubbles (firmware/node and firmware/master www/index.html):
// both pages carry the same CSS and JS snippet, and that JS, run against a small DOM stub,
// opens and closes a note as a mouse, a finger and a keyboard expect. Run: node test_web_bubble.mjs
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

let fails = 0, oks = 0;
const check = (name, cond) => { if (cond) oks++; else { console.log("FAIL: " + name); fails++; } };

const page = rel => readFileSync(fileURLToPath(new URL("../" + rel, import.meta.url)), "utf8");
const pages = { node: page("firmware/node/main/www/index.html"), master: page("firmware/master/main/www/index.html") };

// The snippet: from its marker comment to the end of its last rule (CSS) or handler (JS).
function cut(src, start, last, end) {
  const i = src.indexOf(start), k = i < 0 ? -1 : src.indexOf(last, i), j = k < 0 ? -1 : src.indexOf(end, k);
  return j < 0 ? "" : src.slice(i, j + end.length);
}
const css = src => cut(src, "/* Info bubbles (the same snippet", ".note p {", "\n");
const js = src => cut(src, "// Info bubbles (the same snippet", 'document.addEventListener("keydown"', "\n});");
for (const [name, src] of Object.entries(pages)) {
  check(`${name}: has the CSS snippet`, css(src).length > 0);
  check(`${name}: has the JS snippet`, js(src).includes("infoClose"));
}
check("the CSS snippet is the same on both pages", css(pages.node) === css(pages.master));
check("the JS snippet is the same on both pages", js(pages.node) === js(pages.master));

// A DOM just big enough for the snippet: closest/contains, attributes, hidden, focus + focusin.
const listeners = {};
const fire = (type, target, extra = {}) => (listeners[type] || []).forEach(f => f({ type, target, ...extra }));
class El {
  constructor(tag, attrs = {}, children = []) {
    this.tag = tag; this.attrs = { ...attrs }; this.children = []; this.parent = null;
    this.hidden = "hidden" in attrs; delete this.attrs.hidden;
    this.style = { props: {}, setProperty(k, v) { this.props[k] = v; } };
    this.rect = { left: 0, width: 0 };
    children.forEach(c => { c.parent = this; this.children.push(c); });
  }
  matches(sel) {
    const [t, c] = sel.split(".");
    return (!t || t === this.tag) && (!c || (this.attrs.class || "").split(/\s+/).includes(c));
  }
  closest(sel) { for (let e = this; e; e = e.parent) if (e.matches(sel)) return e; return null; }
  contains(o) { for (let e = o; e; e = e.parent) if (e === this) return true; return false; }
  getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getBoundingClientRect() { return this.rect; }
  focus() { if (doc.activeElement !== this) { doc.activeElement = this; fire("focusin", this); } }
}
const row = (id, left) => {
  const btn = new El("button", { type: "button", class: "info", "aria-expanded": "false", "aria-controls": id });
  btn.rect = { left, width: 24 };
  const text = new El("b");
  const note = new El("div", { id, class: "note", hidden: "" }, [new El("p"), new El("p", {}, [text])]);
  note.rect = { left: 16, width: 298 };
  const input = new El("input");
  return { btn, note, text, input, el: new El("div", { class: "field infobox" }, [new El("span", {}, [btn]), input, note]) };
};
const a = row("a_note", 126), b = row("b_note", 60);
const other = new El("input");
const body = new El("body", {}, [a.el, b.el, other]);
const doc = {
  body, activeElement: body,
  addEventListener(t, f) { (listeners[t] ||= []).push(f); },
  getElementById(id) {
    const walk = e => e.attrs.id === id ? e : e.children.map(walk).find(Boolean);
    return walk(body);
  },
};
new Function("document", js(pages.node))(doc);

const open = r => !r.note.hidden && r.btn.getAttribute("aria-expanded") === "true";
const closed = r => r.note.hidden && r.btn.getAttribute("aria-expanded") === "false";
const tap = (t, focuses = true) => { fire("pointerdown", t); if (focuses) t.focus(); fire("click", t); };
const key = k => fire("keydown", doc.activeElement, { key: k });

check("starts closed", closed(a) && closed(b));
tap(a.btn);
check("a tap opens it", open(a));
check("its arrow points at the button", a.note.style.props["--arrow"] === "122px");
tap(a.btn);
check("a second tap closes it", closed(a));

a.btn.focus(); fire("click", a.btn);
check("Enter/Space (a click with no pointer) opens it", open(a));
a.input.focus();
check("Tab to another control closes it (it would cover the ones under it)", closed(a));

tap(a.btn); tap(a.text, false);
check("a tap inside the note keeps it", open(a));
tap(other);
check("a tap elsewhere closes it", closed(a));

tap(a.btn); tap(b.btn);
check("another bubble's button opens that one and closes this one", closed(a) && open(b));
tap(b.btn);

a.btn.focus(); fire("click", a.btn); key("Escape");
check("Escape closes it", closed(a));
check("Escape from its button leaves the focus there", doc.activeElement === a.btn);

other.focus(); tap(a.btn, false);   // Safari: a clicked button takes no focus
check("a click that leaves the focus on another control opens it", open(a));
key("Escape");
check("Escape then leaves the focus where the user is", closed(a) && doc.activeElement === other);

doc.activeElement = body; tap(a.btn, false); key("Escape");
check("Escape with the focus nowhere puts it on the button", closed(a) && doc.activeElement === a.btn);
key("Escape");
check("Escape with nothing open does nothing", closed(a) && closed(b) && doc.activeElement === a.btn);

console.log(fails ? `${fails} FAILED, ${oks} ok` : `all ${oks} checks ok`);
process.exit(fails ? 1 : 0);
