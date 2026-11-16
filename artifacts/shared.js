// Dantagger explainer: shared helpers (depth switch, tooltips, canvas, formatting, theme changes).
(function () {
  const DT = (window.DT = {});

  // ---- depth: 1 Intuition, 2 Mechanism, 3 Exact ---------------------------
  const listeners = [];
  DT.level = 1;
  try { DT.level = Math.min(3, Math.max(1, parseInt(localStorage.getItem("dt-depth") || "1", 10) || 1)); } catch (e) {}
  DT.onDepth = (fn) => listeners.push(fn);
  DT.setDepth = function (lvl) {
    DT.level = lvl;
    document.body.dataset.level = String(lvl);
    document.querySelectorAll("[data-depth]").forEach((el) => {
      el.hidden = lvl < parseInt(el.dataset.depth, 10);
    });
    document.querySelectorAll(".depth .seg button").forEach((b) => b.setAttribute("aria-pressed", String(+b.dataset.level === lvl)));
    try { localStorage.setItem("dt-depth", String(lvl)); } catch (e) {}
    listeners.forEach((fn) => fn(lvl));
  };

  // ---- theme: colours read from CSS tokens; redraw on change ---------------
  const themeListeners = [];
  DT.onTheme = (fn) => themeListeners.push(fn);
  DT.color = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim() || "#888";
  DT.chColor = (c) => DT.color("--ch" + c);
  function themeChanged() { themeListeners.forEach((fn) => fn()); }
  try { matchMedia("(prefers-color-scheme: dark)").addEventListener("change", themeChanged); } catch (e) {}
  new MutationObserver(themeChanged).observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });

  // ---- tooltip -------------------------------------------------------------
  let tip;
  DT.tip = function (html, x, y) {
    if (!tip) { tip = document.createElement("div"); tip.className = "tip"; tip.setAttribute("role", "tooltip"); document.body.appendChild(tip); }
    if (html == null) { tip.classList.remove("on"); return; }
    tip.innerHTML = html;
    tip.classList.add("on");
    const r = tip.getBoundingClientRect();
    let left = x + 14, top = y + 14;
    if (left + r.width > innerWidth - 8) left = x - r.width - 14;
    if (top + r.height > innerHeight - 8) top = y - r.height - 14;
    tip.style.left = Math.max(8, left) + "px";
    tip.style.top = Math.max(8, top) + "px";
  };

  // ---- canvas: crisp at any DPR, sized to its box --------------------------
  DT.canvas = function (cv, cssHeight) {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(10, cv.parentElement.clientWidth);
    const h = cssHeight;
    cv.style.width = w + "px";
    cv.style.height = h + "px";
    cv.width = Math.round(w * dpr);
    cv.height = Math.round(h * dpr);
    const ctx = cv.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.font = "12px " + getComputedStyle(document.body).getPropertyValue("--f-mono");
    return { ctx, w, h };
  };
  const resizers = [];
  DT.onResize = (fn) => resizers.push(fn);
  let rt;
  addEventListener("resize", () => { clearTimeout(rt); rt = setTimeout(() => resizers.forEach((f) => f()), 120); });

  // ---- formatting ----------------------------------------------------------
  DT.fmtTime = function (ps, digits = 3) {
    const a = Math.abs(ps);
    const units = [[8.64e16, "days"], [3.6e15, "h"], [1e12, "s"], [1e9, "ms"], [1e6, "µs"], [1e3, "ns"], [1, "ps"]];
    for (const [f, u] of units) if (a >= f || f === 1) return String(Number((ps / f).toPrecision(digits))) + " " + u;
  };
  DT.fmtRate = function (hz, digits = 3) {
    const a = Math.abs(hz);
    if (a >= 1e6) return trim((hz / 1e6).toPrecision(digits)) + " M/s";
    if (a >= 1e3) return trim((hz / 1e3).toPrecision(digits)) + " k/s";
    return trim(hz.toPrecision(digits)) + " /s";
  };
  DT.fmtInt = (n) => Number(n).toLocaleString("en-US");
  function trim(s) { return s.includes("e") ? s : s.replace(/(\.\d*?)0+$/, "$1").replace(/\.$/, ""); }
  DT.hex = (v, digits) => "0x" + v.toString(16).toUpperCase().padStart(digits, "0");

  // ---- deterministic random numbers ----------------------------------------
  DT.rng = function (seed) {
    let a = seed >>> 0;
    const next = () => {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
    next.exp = (mean) => -Math.log(1 - next()) * mean;
    next.gauss = () => { let u = 0, v = 0; while (u === 0) u = next(); v = next(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); };
    return next;
  };

  // ---- boot ----------------------------------------------------------------
  document.addEventListener("DOMContentLoaded", () => {
    document.querySelectorAll(".depth .seg button").forEach((b) => b.addEventListener("click", () => DT.setDepth(+b.dataset.level)));
    DT.setDepth(DT.level);
  });
})();
