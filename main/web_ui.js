(function (root) {
  "use strict";

  function frame8n1(text) {
    const bytes = new TextEncoder().encode(String(text));
    const bits = [];
    for (const byte of bytes) {
      bits.push(0);
      for (let bit = 0; bit < 8; bit++) bits.push((byte >> bit) & 1);
      bits.push(1);
    }
    return bits;
  }

  function waveformPath(bits, bitWidth, highY, lowY) {
    if (!bits.length) return { path: "", width: 0 };
    const width = bitWidth || 10;
    const high = highY === undefined ? 4 : highY;
    const low = lowY === undefined ? 20 : lowY;
    let y = bits[0] ? high : low;
    let path = `M0 ${y}`;
    for (let i = 0; i < bits.length; i++) {
      const nextY = bits[i] ? high : low;
      if (i && nextY !== y) path += ` V${nextY}`;
      path += ` H${(i + 1) * width}`;
      y = nextY;
    }
    return { path, width: bits.length * width };
  }

  function drawDividers(document) {
    for (const svg of document.querySelectorAll(".mypi-divider svg[data-text]")) {
      const text = svg.getAttribute("data-text");
      const bits = frame8n1(text);
      const geometry = waveformPath(bits, 10, 4, 20);
      svg.setAttribute("viewBox", `0 0 ${geometry.width} 24`);
      svg.setAttribute("preserveAspectRatio", "none");
      svg.setAttribute("role", "img");
      svg.setAttribute("aria-label", `${text}, framed as 8N1 UART data`);
      svg.dataset.bitCount = String(bits.length);
      const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
      path.setAttribute("d", geometry.path);
      svg.replaceChildren(path);
    }
  }

  // Dashboard pages: one HTML shell, sections selected by location.hash (no extra URI handlers).
  const PAGES = ["overview", "wifi", "access-point", "traffic", "ethernet", "system", "debug"];
  const PAGE_ALIASES = { diagnostics: "debug", status: "overview", clients: "access-point" };

  function pageFromHash(hash) {
    const key = String(hash || "").replace(/^#/, "").toLowerCase();
    const page = PAGE_ALIASES[key] || key;
    return PAGES.includes(page) ? page : PAGES[0];
  }

  function showPage(document, hash) {
    const sections = document.querySelectorAll("section.page[data-page]");
    if (!sections.length) return null;   // Standalone pages (/update, /c5update, /pins)
    const page = pageFromHash(hash);
    for (const section of sections) section.hidden = section.dataset.page !== page;
    for (const link of document.querySelectorAll(".mypi-nav a")) {
      const active = link.getAttribute("href") === `/#${page}`;
      link.classList.toggle("active", active);
      if (active) link.setAttribute("aria-current", "page");
      else link.removeAttribute("aria-current");
    }
    return page;
  }

  // STA RSSI: same 20-step red (<= -80 dBm) to green (>= -40 dBm) gradient as the LCD icon.
  function rssiStep(rssi) {
    const step = Math.trunc(((rssi + 80) * 19) / 40);
    return Math.min(19, Math.max(0, step));
  }

  function rssiColor(rssi) {
    const step = rssiStep(rssi);
    return `rgb(${Math.trunc((255 * (19 - step)) / 19)},${Math.trunc((255 * step) / 19)},0)`;
  }

  function staSignal(status) {
    const state = String((status && status.wifi_state) || "");
    const up = state === "connected" || state.startsWith("associated");
    const rssi = Number(status && status.rssi);
    if (!up || !Number.isFinite(rssi) || rssi >= 0) return { text: "\u2014", color: "" };
    return { text: `${Math.round(rssi)} dBm`, color: rssiColor(rssi) };
  }

  async function updateWifi(document, fetchFn) {
    const box = document.getElementById("mypi-wifi");
    const value = document.getElementById("mypi-dbm");
    if (!box || !value) return;
    let signal = { text: "\u2014", color: "" };
    try {
      signal = staSignal(await (await fetchFn("/api/status")).json());
    } catch (e) {
      // Keep the placeholder while the device is unreachable.
    }
    value.textContent = signal.text;
    box.style.color = signal.color;
  }

  function start(document) {
    drawDividers(document);
    if (showPage(document, root.location && root.location.hash)) {
      root.addEventListener("hashchange", () => showPage(document, root.location.hash));
    }
    if (typeof root.fetch === "function") {
      updateWifi(document, root.fetch.bind(root));
      root.setInterval(() => updateWifi(document, root.fetch.bind(root)), 5000);
    }
  }

  const api = { frame8n1, waveformPath, drawDividers, pageFromHash, showPage, rssiStep, rssiColor, staSignal };
  root.MyPiSerial = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  if (root.document) {
    if (root.document.readyState === "loading") {
      root.document.addEventListener("DOMContentLoaded", () => start(root.document), { once: true });
    } else {
      start(root.document);
    }
  }
})(globalThis);
