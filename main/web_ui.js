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
  const PAGES = ["overview", "wifi", "access-point", "traffic", "ethernet", "system", "camera", "optimize", "debug"];
  const PAGE_ALIASES = { diagnostics: "debug", status: "overview", clients: "access-point", opt: "optimize", photo: "camera" };

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

  // Only the main page shell refreshes the header RSSI periodically. Standalone pages
  // (/update, /c5update, /pins) read it once: the single-task HTTP server is busy with
  // the upload during OTA, and extra requests would only queue up behind it.
  function shouldPollStatus(isShell, hidden) {
    return !!isShell && !hidden;
  }

  // Show/hide toggle (v0.8.8): flips one input between type="password" and type="text".
  function togglePassword(input, button) {
    const show = input.type === "password";
    input.type = show ? "text" : "password";
    button.textContent = show ? "\u{1F512} Hide" : "\u{1F441} Show";
    button.setAttribute("aria-pressed", show ? "true" : "false");
    button.title = show ? "Hide password" : "Show password";
    return show;
  }

  // Adds a small Show/Hide button right after every password field on the page.
  function addPasswordToggles(document) {
    const inputs = Array.from(document.querySelectorAll("input[type=password]"));
    for (const input of inputs) {
      if (input.dataset.pwToggle) continue;
      input.dataset.pwToggle = "1";
      const button = document.createElement("button");
      button.type = "button";
      button.className = "pw-toggle";
      button.textContent = "\u{1F441} Show";
      button.title = "Show password";
      button.setAttribute("aria-pressed", "false");
      if (input.id) button.setAttribute("aria-controls", input.id);
      button.addEventListener("click", () => togglePassword(input, button));
      input.insertAdjacentElement("afterend", button);
    }
    return inputs.length;
  }

  // --- Optimize page (v0.9.0, trimmed v0.9.3): GET/POST /api/opt + /api/radio, POST /api/clear_emergency ---
  // Same field names and defaults as main/opt_config_core.h. Band and the shared STA/AP protocol mask
  // live in the radio settings (/api/radio). v0.9.4: TCP window, LwIP core and NAPT settings are back.
  // v0.9.5: 5 GHz width (bw_5g 40|20, 2.4 GHz always HT20) and SDIO clock (sdio_mhz 40|50, auto-fallback).
  const OPT_FIELDS = ["wifi_tx_power_dbm", "ps_mode", "ap_beacon_ms", "country",
    "tcp_wnd_kb", "lwip_core", "napt_max", "napt_tcp_to", "bw_5g", "sdio_mhz"];
  const OPT_PS_NAMES = ["Off", "Min modem", "Max modem"];
  const OPT_CORE_NAMES = ["Auto (no pin)", "Core 0", "Core 1"];
  const OPT_NAPT_MAX_ENTRIES = { 5: 512, 10: 1024, 20: 2048 };
  const OPT_NAPT_TCP_TO_NAMES = ["30 min", "1 h", "2 h"];
  const OPT_BAND_NAMES = ["Both (2.4 + 5 GHz)", "2.4 GHz only", "5 GHz only"];
  // Same bits as RADIO_PROTO_* in main/wifi.h (ax allowed again since v0.9.3, known C5 fw issue).
  const PROTO_BITS = { a: 16, b: 1, g: 2, n: 4, ac: 32, ax: 64 };
  const PROTO_ALL = 0x77;
  const PROTO_2G = 0x47;   // b/g/n/ax
  const PROTO_5G = 0x74;   // a/n/ac/ax

  function optDefaults() {
    return { wifi_tx_power_dbm: 0, ps_mode: 0, ap_beacon_ms: 100, country: "SE",
      tcp_wnd_kb: 32, lwip_core: 0, napt_max: 5, napt_tcp_to: 2, bw_5g: 40, sdio_mhz: 40 };
  }

  // AP beacon interval, country, TCP window, LwIP core, NAPT max sessions, 5 GHz width and SDIO clock are only read at boot
  // (same rule as opt_config_reboot_required()); the NAPT TCP timeout applies immediately.
  function optRebootRequired(active, saved) {
    return !!active && !!saved &&
      (Number(active.ap_beacon_ms) !== Number(saved.ap_beacon_ms) ||
       String(active.country) !== String(saved.country) ||
       Number(active.tcp_wnd_kb) !== Number(saved.tcp_wnd_kb) ||
       Number(active.lwip_core) !== Number(saved.lwip_core) ||
       Number(active.napt_max) !== Number(saved.napt_max) ||
       Number(active.bw_5g) !== Number(saved.bw_5g) ||
       Number(active.sdio_mhz) !== Number(saved.sdio_mhz));
  }

  /** Protocol bitmask from the a/b/g/n/ac/ax checkboxes (0 = none checked = default). */
  function protoMaskFromChecks(document) {
    let m = 0;
    for (const [k, bit] of Object.entries(PROTO_BITS)) {
      const e = document.getElementById("opt-proto-" + k);
      if (e && e.checked) m |= bit;
    }
    return m & PROTO_ALL;
  }

  function protoSetChecks(document, mask) {
    for (const [k, bit] of Object.entries(PROTO_BITS)) {
      const e = document.getElementById("opt-proto-" + k);
      if (e) e.checked = (Number(mask) & bit) !== 0;
    }
  }

  /** The part of a mask that the selected band can use (0 = both bands, 1 = 2.4 GHz, 2 = 5 GHz). */
  function protoFilterForBand(mask, band) {
    const m = Number(mask) & PROTO_ALL;
    return Number(band) === 1 ? m & PROTO_2G : Number(band) === 2 ? m & PROTO_5G : m;
  }

  function protoLabel(mask) {
    const m = Number(mask) & PROTO_ALL;
    if (!m) return "default (b/g/n + a/n/ac)";
    return Object.entries(PROTO_BITS).filter(([, bit]) => m & bit).map(([k]) => k).join("/");
  }

  /** Status text for the protocol row; warns about ax and about a band left without protocols. */
  function protoHint(mask, band) {
    const m = Number(mask) & PROTO_ALL;
    const parts = [protoLabel(m)];
    if (m && Number(band) !== 2 && !(m & PROTO_2G)) parts.push("no 2.4 GHz protocol checked - 2.4 GHz uses default b/g/n");
    if (m && Number(band) !== 1 && !(m & PROTO_5G)) parts.push("no 5 GHz protocol checked - 5 GHz uses default a/n/ac");
    if (m & PROTO_BITS.ax) parts.push("ax: known C5 issue, upload may stall");
    return parts.join("; ");
  }

  const OPT_SELECTS = [["opt-tx", "wifi_tx_power_dbm"], ["opt-ps", "ps_mode"], ["opt-beacon", "ap_beacon_ms"],
    ["opt-country", "country"], ["opt-tcp-wnd", "tcp_wnd_kb"], ["opt-lwip-core", "lwip_core"],
    ["opt-napt-max", "napt_max"], ["opt-napt-tcp-to", "napt_tcp_to"], ["opt-bw-5g", "bw_5g"], ["opt-sdio", "sdio_mhz"]];

  function optFillForm(document, d) {
    for (const [id, k] of OPT_SELECTS) {
      const e = document.getElementById(id);
      if (e && d[k] !== undefined) e.value = String(d[k]);
    }
  }

  /** Only the controls that have a value end up in the POST body (missing keys keep the saved value). */
  function optReadForm(document) {
    const body = {};
    for (const [id, k] of OPT_SELECTS) {
      const e = document.getElementById(id);
      if (e && e.value !== "") body[k] = k === "country" ? String(e.value) : Number(e.value);
    }
    return body;
  }

  /** Radio part of the form (band + shared protocol mask) from a GET /api/radio reply. */
  function radioFillForm(document, r) {
    const b = document.getElementById("opt-band");
    if (b && r && r.band_mode !== undefined) b.value = String(r.band_mode);
    if (r && r.proto !== undefined) protoSetChecks(document, r.proto);
  }

  /** POST /api/radio body: the band and one protocol mask that STA and AP share. */
  function radioReadForm(document) {
    const body = { proto: protoMaskFromChecks(document) };
    const b = document.getElementById("opt-band");
    if (b && b.value !== "") body.band_mode = Number(b.value);
    return body;
  }

  /** Right-hand status text per row, from a GET /api/opt reply (and optionally GET /api/radio). */
  function optStatus(d, r) {
    const a = d.active || d;
    const pend = (k) => (a[k] !== d[k] ? " - saved " + d[k] + ", reboot pending" : "");
    const st = {
      "opt-tx-st": a.wifi_tx_power_dbm ? "cap " + a.wifi_tx_power_dbm + " dBm" : "driver default (max)",
      "opt-ps-st": (OPT_PS_NAMES[a.ps_mode] || "?") + " - Recommended: Off in AP+STA mode",
      "opt-beacon-st": "active " + a.ap_beacon_ms + " ms" + pend("ap_beacon_ms"),
      "opt-country-st": "active " + a.country + pend("country"),
      "opt-tcp-wnd-st": "active " + a.tcp_wnd_kb + " KB" + pend("tcp_wnd_kb"),
      "opt-lwip-core-st": "active " + (OPT_CORE_NAMES[a.lwip_core] || "?") + pend("lwip_core"),
      "opt-napt-max-st": "active " + (OPT_NAPT_MAX_ENTRIES[a.napt_max] || "?") + " sessions" + pend("napt_max"),
      "opt-napt-tcp-to-st": "active " + (OPT_NAPT_TCP_TO_NAMES[a.napt_tcp_to] || "?"),
      "opt-bw-5g-st": "active 5 GHz HT" + a.bw_5g + (Number(a.bw_5g) === 40 ? " (11n, ac/ax off on 5 GHz)" : "") +
        " (2.4 GHz always HT20)" +
        (Number(a.bw_5g) !== Number(d.bw_5g) ? " - saved HT" + d.bw_5g + ", reboot pending" : ""),
      "opt-sdio-st": sdioStatus(d),
      "opt-emerg-st": (d.emergency_active ? "SET - emergency mode was entered after a C5 failure" : "clear") +
        (d.emergency_mode_now ? " (emergency mode active now)" : "") + "; auto-clears on successful boot",
    };
    if (r) {
      const ra = r.active || r, rs = r.saved || r;
      st["opt-band-st"] = "active " + (OPT_BAND_NAMES[ra.band_mode] || "?") +
        (ra.band_mode !== rs.band_mode ? " - saved " + (OPT_BAND_NAMES[rs.band_mode] || "?") + ", reboot pending" : "");
      st["opt-proto-st"] = "active " + protoHint(ra.proto, ra.band_mode) +
        (ra.proto !== rs.proto ? " - saved " + protoLabel(rs.proto) + ", reboot pending" : "");
      if (r.info && r.info.sta_width && Number(r.sta_channel) > 14) st["opt-bw-5g-st"] += "; STA link now " + r.info.sta_width;
    }
    return st;
  }

  /** SDIO row: clock in use this boot (real bus clock if known), fallback notice, pending change. */
  function sdioStatus(d) {
    const mhz = d.sdio_active_mhz !== undefined ? Number(d.sdio_active_mhz) : Number((d.active || d).sdio_mhz);
    let t = "active " + mhz + " MHz";
    if (d.sdio_real_khz) t += " (bus " + (Number(d.sdio_real_khz) / 1000) + " MHz)";
    if (d.sdio_fell_back) t += " - fell back from 50 MHz after a C5 failure";
    else if (d.sdio_mhz !== undefined && Number(d.sdio_mhz) !== mhz) t += " - saved " + d.sdio_mhz + " MHz, reboot pending";
    return t;
  }

  function radioRebootRequired(r) {
    if (!r) return false;
    const ra = r.active || r, rs = r.saved || r;
    return ra.band_mode !== rs.band_mode || ra.proto !== rs.proto;
  }

  function optShow(document, d, r) {
    for (const [id, text] of Object.entries(optStatus(d, r))) {
      const el = document.getElementById(id);
      if (el) el.textContent = text;
    }
    const st = document.getElementById("opt-emerg-st");
    if (st && st.classList) st.classList.toggle("bad", !!d.emergency_active);
    const btn = document.getElementById("opt-emerg-btn");
    if (btn) btn.disabled = !d.emergency_active;
    optBanner(document, !!d.reboot_required || radioRebootRequired(r));
  }

  function optBanner(document, on) {
    const b = document.getElementById("opt-banner");
    if (b) b.hidden = !on;
  }

  function optMsg(document, text) {
    const m = document.getElementById("opt-msg");
    if (m) m.textContent = text;
  }

  async function optLoad(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    try {
      const d = await (await f("/api/opt")).json();
      let r = null;
      try { r = await (await f("/api/radio")).json(); } catch (e) { r = null; }
      optFillForm(doc, d);
      if (r) radioFillForm(doc, r);
      optShow(doc, d, r);
      return d;
    } catch (e) {
      optMsg(doc, "Device unreachable");
      return null;
    }
  }

  async function optPost(f, url, body) {
    const r = await f(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
    return r.json();
  }

  async function optSave(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    try {
      const jr = doc.getElementById("opt-band") ? await optPost(f, "/api/radio", radioReadForm(doc)) : { ok: true };
      if (!jr.ok) {
        optMsg(doc, "ERROR (radio) " + (jr.error || ""));
        return jr;
      }
      const j = await optPost(f, "/api/opt", optReadForm(doc));
      optMsg(doc, j.ok ? "Saved" + (j.reboot_required ? " - reboot required" : " - band/protocol changes apply after reboot")
        : "ERROR " + (j.error || ""));
      if (j.ok) await optLoad(doc, f);
      return j;
    } catch (e) {
      optMsg(doc, "Save failed: " + e);
      return null;
    }
  }

  async function optClearEmergency(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    try {
      const j = await optPost(f, "/api/clear_emergency");
      optMsg(doc, j.ok ? "Emergency flag cleared" : "ERROR " + (j.error || ""));
      if (j.ok) await optLoad(doc, f);
      return j;
    } catch (e) {
      optMsg(doc, "Clear failed: " + e);
      return null;
    }
  }

  // Camera page (v0.10.0): status + saved photos from /api/camera/status, preview = one JPEG at a time
  // from /api/camera/frame.jpg while the page is visible (the firmware stops the stream when idle),
  // shutter = POST /api/camera/capture. Errors name the failing stage (camera, capture, storage).
  const CAMERA_PREVIEW_MS = 300, CAMERA_RETRY_MS = 3000;
  const CAMERA_STAGES = { camera: "Camera", capture: "Capture", storage: "Storage" };
  const CAMERA_PHOTO_RE = /^IMG\d{5}\.JPG$/;
  const CAMERA_NUMBERS = ["preview_quality", "photo_quality", "idle_stop_s", "max_photos", "reserve_kb"];
  const CAMERA_BOOLEANS = ["hflip", "vflip", "live_preview"];
  const cameraForms = new WeakMap();

  function cameraSettingsFill(doc, settings, caps) {
    for (const key of CAMERA_NUMBERS) {
      const el = doc.getElementById("cam-" + key);
      if (el) el.value = String(settings[key]);
    }
    for (const key of CAMERA_BOOLEANS) {
      const el = doc.getElementById("cam-" + key);
      if (el) {
        el.checked = !!settings[key];
        if (key !== "live_preview") el.disabled = !caps[key];
      }
    }
    const select = doc.getElementById("cam-resolution");
    if (select) {
      select.replaceChildren();
      for (const mode of caps.modes || []) {
        const option = doc.createElement("option");
        option.value = `${mode.width}x${mode.height}`;
        option.textContent = option.value;
        select.appendChild(option);
      }
      select.value = `${settings.width}x${settings.height}`;
      select.disabled = (caps.modes || []).length < 2;
    }
    camSet(doc, "cam-mode-note", (caps.modes || []).length < 2 ?
      "Current firmware sensor mode only; no synthetic resolutions." : "Driver-supported modes only.");
  }

  async function cameraSettingsLoad(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    const form = doc.getElementById("cam-settings");
    try {
      const response = await f("/api/camera/settings", { cache: "no-store" });
      const data = await response.json();
      if (!response.ok || !data.settings || !data.capabilities) throw new Error();
      cameraForms.set(doc, data);
      cameraSettingsFill(doc, data.settings, data.capabilities);
      if (form) form.disabled = false;
      return data;
    } catch (e) {
      if (form) form.disabled = true;
      camSet(doc, "cam-settings-msg", "Camera settings unavailable", true);
      return null;
    }
  }

  async function cameraSettingsSave(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    if (!cameraForms.has(doc)) return null;
    const settings = {};
    for (const key of CAMERA_NUMBERS) {
      const input = doc.getElementById("cam-" + key);
      if (!input.value.trim() || (input.checkValidity && !input.checkValidity())) {
        camSet(doc, "cam-settings-msg", "Check the setting ranges", true);
        return null;
      }
      settings[key] = Number(input.value);
      if (!Number.isInteger(settings[key])) return null;
    }
    for (const key of CAMERA_BOOLEANS) settings[key] = doc.getElementById("cam-" + key).checked;
    const [width, height] = doc.getElementById("cam-resolution").value.split("x").map(Number);
    settings.width = width;
    settings.height = height;
    try {
      const response = await f("/api/camera/settings", {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(settings),
      });
      const result = await response.json();
      if (!response.ok || !result.ok) throw new Error();
      await cameraSettingsLoad(doc, f);
      await cameraLoad(doc, f);
      camSet(doc, "cam-settings-msg", "Saved and applied", false);
      if (cameraVisible(doc) && cameraLiveEnabled(doc)) cameraPreview(doc, f);
      return result;
    } catch (e) {
      camSet(doc, "cam-settings-msg", "Could not save camera settings; check supported values and retry", true);
      return null;
    }
  }

  function cameraSettingsReset(document) {
    const doc = document || root.document, data = cameraForms.get(doc);
    if (!data) return;
    cameraSettingsFill(doc, data.defaults, data.capabilities);
    camSet(doc, "cam-settings-msg", "Defaults loaded; Save and apply to keep them");
    cameraLiveChanged(doc);
  }

  function cameraLiveEnabled(doc) {
    const input = doc.getElementById("cam-live_preview");
    return !input || input.checked;
  }

  function cameraLiveChanged(document) {
    const doc = document || root.document;
    if (cameraLiveEnabled(doc) && cameraVisible(doc)) cameraPreview(doc);
  }

  // Decode only the WIFI payload subset the STA profile supports. Never include raw credentials
  // in error messages, URLs, logs, HTML, or persistent browser storage.
  function parseWifiQr(raw) {
    const invalid = () => { throw new Error("Invalid or unsupported Wi-Fi QR (use WPA/WPA2 or an open, visible network)"); };
    if (typeof raw !== "string" || raw.length > 2048 || !raw.startsWith("WIFI:")) return invalid();
    const fields = Object.create(null);
    let part = "", escaped = false;
    for (const char of raw.slice(5)) {
      if (escaped) {
        if (!"\\;,:\"".includes(char)) return invalid();
        part += char;
        escaped = false;
      } else if (char === "\\") {
        escaped = true;
      } else if (char === ";") {
        if (part) {
          const colon = part.indexOf(":");
          const key = part.slice(0, colon);
          if (colon < 1 || Object.hasOwn(fields, key)) return invalid();
          fields[key] = part.slice(colon + 1);
        }
        part = "";
      } else {
        part += char;
      }
    }
    if (escaped || part) return invalid();
    const ssid = fields.S, security = fields.T || "nopass", pass = fields.P || "";
    const bytes = (value) => new TextEncoder().encode(value).length;
    if (!ssid || bytes(ssid) > 32 || /[\u0000-\u001f\u007f]/.test(ssid + pass)) return invalid();
    if (fields.H && fields.H.toLowerCase() !== "false") return invalid();
    if (!["WPA", "WPA2", "nopass"].includes(security)) return invalid();
    if (security !== "nopass" &&
        !((bytes(pass) >= 8 && bytes(pass) <= 63) || /^[0-9a-fA-F]{64}$/.test(pass))) return invalid();
    if (security === "nopass" && pass) return invalid();
    return { ssid, pass, security };
  }

  const wifiQrCandidates = new WeakMap();
  const wifiQrBusy = new WeakSet();
  const wifiQrGeneration = new WeakMap();

  function wifiQrClear(document) {
    const doc = document || root.document;
    wifiQrGeneration.set(doc, (wifiQrGeneration.get(doc) || 0) + 1);
    wifiQrCandidates.delete(doc);
    for (const id of ["qr-password", "qr-payload"]) {
      const input = doc.getElementById(id);
      if (input) input.value = "";
    }
    const review = doc.getElementById("qr-review"), fill = doc.getElementById("qr-fill");
    if (review) review.hidden = true;
    if (fill) fill.disabled = true;
    camSet(doc, "qr-ssid", "");
    camSet(doc, "qr-security", "");
    const img = doc.getElementById("qr-img");
    if (img && img.dataset.url) {
      root.URL.revokeObjectURL(img.dataset.url);
      delete img.dataset.url;
      img.removeAttribute("src");
    }
  }

  function wifiQrReview(doc, raw) {
    const candidate = parseWifiQr(raw);
    wifiQrCandidates.set(doc, candidate);
    camSet(doc, "qr-ssid", candidate.ssid);
    camSet(doc, "qr-security", candidate.security === "nopass" ? "Open" : candidate.security);
    const password = doc.getElementById("qr-password");
    password.type = "password";
    password.value = candidate.pass;
    doc.getElementById("qr-review").hidden = false;
    doc.getElementById("qr-fill").disabled = false;
    camSet(doc, "qr-msg", "QR detected. Review before filling the selected STA profile. Nothing has been saved.", false);
    return candidate;
  }

  function wifiQrParse(document) {
    const doc = document || root.document;
    const raw = doc.getElementById("qr-payload").value;
    wifiQrClear(doc);
    try {
      return wifiQrReview(doc, raw);
    } catch (e) {
      camSet(doc, "qr-msg", "Invalid or unsupported Wi-Fi QR text", true);
      return null;
    }
  }

  async function wifiQrRead(document, fetchFn, decoder) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    if (wifiQrBusy.has(doc)) return null;
    wifiQrBusy.add(doc);
    wifiQrClear(doc);
    const generation = wifiQrGeneration.get(doc);
    const button = doc.getElementById("qr-read");
    if (button) button.disabled = true;
    camSet(doc, "qr-msg", "Reading a fresh image from the MyPi camera...");
    let bitmap;
    try {
      const response = await f(`/api/camera/frame.jpg?qr=1&t=${Date.now()}`, { cache: "no-store" });
      if (!response.ok) throw new Error();
      const blob = await response.blob(), img = doc.getElementById("qr-img");
      if (wifiQrGeneration.get(doc) !== generation) return null;
      const url = root.URL.createObjectURL(blob);
      img.dataset.url = url;
      img.src = url;
      if (!decoder) {
        if (!root.BarcodeDetector || !root.createImageBitmap ||
            !(await root.BarcodeDetector.getSupportedFormats()).includes("qr_code")) {
          camSet(doc, "qr-msg", "This browser cannot decode QR here (native decoding may require HTTPS). Use the camera image and manual fallback below.", true);
          return null;
        }
        decoder = new root.BarcodeDetector({ formats: ["qr_code"] });
      }
      bitmap = await root.createImageBitmap(blob);
      const codes = await decoder.detect(bitmap);
      if (wifiQrGeneration.get(doc) !== generation) return null;
      if (codes.length !== 1) {
        camSet(doc, "qr-msg", "Show exactly one Wi-Fi QR code to the camera, then try again.", true);
        return null;
      }
      return wifiQrReview(doc, codes[0].rawValue);
    } catch (e) {
      if (wifiQrGeneration.get(doc) !== generation) return null;
      camSet(doc, "qr-msg", "Could not read a supported Wi-Fi QR. Check the camera, try again, or use manual fallback.", true);
      return null;
    } finally {
      if (bitmap) bitmap.close();
      wifiQrBusy.delete(doc);
      if (button) button.disabled = false;
    }
  }

  function wifiQrFill(document) {
    const doc = document || root.document, candidate = wifiQrCandidates.get(doc);
    if (!candidate) return false;
    doc.getElementById("sn").value = candidate.ssid;
    const password = doc.getElementById("spw");
    password.type = "password";
    password.value = candidate.pass;
    doc.getElementById("sclr").checked = candidate.security === "nopass";
    wifiQrClear(doc);
    camSet(doc, "qr-msg", "Selected STA profile filled, not saved. Review its slot and IP settings, then Save; Connect is separate.", false);
    return true;
  }

  function cameraUsable(s) {
    return !!(s && s.enabled && ["ready", "streaming", "error"].includes(s.state));
  }

  function cameraStatusText(s) {
    if (!s) return "Device unreachable";
    if (!s.enabled) return "Camera support is disabled in this firmware (menuconfig: MyPi-Hotspot > Camera)";
    if (s.state === "starting") return "Camera starting...";
    if (s.state === "not_detected") return "Camera not detected" + (s.error ? `: ${s.error}` : "");
    const base = `${s.sensor || "Camera"} ${s.width}x${s.height}`;
    if (s.state === "error") return `${base} - error: ${s.error || "unknown"} (retrying)`;
    return `${base} - ${s.state === "streaming" ? "streaming" : "ready"}`;
  }

  function cameraStorageText(s) {
    const st = s && s.storage;
    if (!st || !st.mounted) return "Storage not mounted: photos cannot be saved";
    return `/store/photos - ${(s.photos || []).length} of max ${s.max_photos} photos, ${st.free_kb} KB free of ${st.total_kb} KB`;
  }

  function cameraResultText(j) {
    if (!j) return "Capture failed: device unreachable";
    if (j.ok) return `Saved ${j.name} (${Math.ceil((j.size || 0) / 1024)} KB)`;
    return `${CAMERA_STAGES[j.stage] || "Camera"} error: ${j.error || "unknown"}`;
  }

  function cameraPhotoRows(photos) {
    const rows = (photos || []).filter((p) => p && CAMERA_PHOTO_RE.test(p.name)).map((p) => {
      const url = `/api/camera/photo?name=${p.name}`;
      return `<tr><td>${p.name}</td><td>${Math.ceil((p.size || 0) / 1024)} KB</td>` +
        `<td><a href='${url}' target='_blank'>view</a> <a href='${url}&dl=1'>download</a> ` +
        `<button type='button' onclick="MyPiSerial.cameraDelete('${p.name}')">delete</button></td></tr>`;
    });
    return rows.length ? "<tr><th>Photo</th><th>Size</th><th></th></tr>" + rows.join("") : "<tr><td>No photos saved yet</td></tr>";
  }

  function camSet(doc, id, text, bad) {
    const el = doc.getElementById(id);
    if (!el) return null;
    el.textContent = text;
    if (bad !== undefined && el.classList) {
      el.classList.toggle("bad", !!bad);
      el.classList.toggle("ok", !bad);
    }
    return el;
  }

  async function cameraLoad(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    let s = null;
    try {
      s = await (await f("/api/camera/status")).json();
    } catch (e) {
      s = null;
    }
    const usable = cameraUsable(s);
    camSet(doc, "cam-st", cameraStatusText(s), !usable || s.state === "error");
    const store = camSet(doc, "cam-store", cameraStorageText(s), !(s && s.storage && s.storage.mounted));
    if (store && !s) store.textContent = "";
    const btn = doc.getElementById("cam-shutter");
    if (btn) btn.disabled = !usable;
    const table = doc.getElementById("cam-photos");
    if (table) table.innerHTML = s ? cameraPhotoRows(s.photos) : "";
    return s;
  }

  async function cameraCapture(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    const btn = doc.getElementById("cam-shutter");
    if (btn) btn.disabled = true;
    camSet(doc, "cam-msg", "Capturing...");
    let j = null;
    try {
      j = await (await f("/api/camera/capture", { method: "POST" })).json();
    } catch (e) {
      j = null;
    }
    camSet(doc, "cam-msg", cameraResultText(j), !(j && j.ok));
    await cameraLoad(doc, f);
    return j;
  }

  async function cameraDelete(name, document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    if (!CAMERA_PHOTO_RE.test(String(name))) return null;
    if (typeof root.confirm === "function" && !root.confirm(`Delete ${name}?`)) return null;
    let j = null;
    try {
      j = await (await f(`/api/camera/delete?name=${name}`, { method: "POST" })).json();
    } catch (e) {
      j = null;
    }
    camSet(doc, "cam-msg", j && j.ok ? `Deleted ${name}` : `Delete failed: ${(j && j.error) || "device unreachable"}`, !(j && j.ok));
    await cameraLoad(doc, f);
    return j;
  }

  function cameraVisible(doc) {
    const section = doc.querySelector && doc.querySelector("section.page[data-page='camera']");
    return !!section && !section.hidden && !doc.hidden;
  }

  // One preview frame into #cam-img. Throws with the firmware's reason text on failure.
  async function cameraFrame(doc, f) {
    const r = await f(`/api/camera/frame.jpg?t=${Date.now()}`, { cache: "no-store" });
    if (!r.ok) throw new Error((await r.text()) || `HTTP ${r.status}`);
    const blob = await r.blob();
    const img = doc.getElementById("cam-img");
    if (!img || !root.URL || !root.URL.createObjectURL) return;
    const old = img.dataset && img.dataset.url;
    const url = root.URL.createObjectURL(blob);
    img.src = url;
    if (img.dataset) img.dataset.url = url;
    if (old) root.URL.revokeObjectURL(old);
  }

  let cameraLoopRunning = false;

  // Sequential preview loop (never more than one request in flight); ends when the page is hidden.
  async function cameraPreview(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    if (cameraLoopRunning) return;
    cameraLoopRunning = true;
    let failed = false;
    try {
      while (cameraVisible(doc) && cameraLiveEnabled(doc)) {
        let delay = CAMERA_PREVIEW_MS;
        try {
          await cameraFrame(doc, f);
          const err = doc.getElementById("cam-err");
          if (err) err.hidden = true;
          if (failed) await cameraLoad(doc, f);
          failed = false;
        } catch (e) {
          const err = camSet(doc, "cam-err", `Preview unavailable: ${e && e.message ? e.message : e}`);
          if (err) err.hidden = false;
          if (!failed) await cameraLoad(doc, f);
          failed = true;
          delay = CAMERA_RETRY_MS;
        }
        await new Promise((resolve) => root.setTimeout(resolve, delay));
      }
    } finally {
      cameraLoopRunning = false;
    }
  }

  let cameraOpening = false, cameraOpenTimer;

  async function cameraOpen(document) {
    if (typeof root.fetch !== "function" || !cameraVisible(document)) return;
    root.clearTimeout(cameraOpenTimer);
    if (cameraOpening) return;
    cameraOpening = true;
    let settings;
    try {
      [, settings] = await Promise.all([cameraLoad(document), cameraSettingsLoad(document)]);
    } finally {
      cameraOpening = false;
    }
    if (settings) cameraPreview(document);
    else if (cameraVisible(document)) {
      camSet(document, "cam-st", "Camera settings unavailable; retrying before starting preview", true);
      cameraOpenTimer = root.setTimeout(() => cameraOpen(document), CAMERA_RETRY_MS);
    }
  }

  function start(document) {
    drawDividers(document);
    addPasswordToggles(document);
    const isShell = !!showPage(document, root.location && root.location.hash);
    if (isShell) {
      root.addEventListener("hashchange", () => {
        const page = showPage(document, root.location.hash);
        if (page !== "wifi") wifiQrClear(document);
        if (page === "optimize" && typeof root.fetch === "function") optLoad(document);
        if (page === "camera") cameraOpen(document);
      });
      if (document.addEventListener) document.addEventListener("visibilitychange", () => {
        if (document.hidden) wifiQrClear(document);
        cameraOpen(document);
      });
      cameraOpen(document);
    }
    if (typeof root.fetch === "function") {
      updateWifi(document, root.fetch.bind(root));
      if (document.getElementById && document.getElementById("opt-form")) optLoad(document);
      if (isShell) {
        root.setInterval(() => {
          if (shouldPollStatus(isShell, document.hidden)) updateWifi(document, root.fetch.bind(root));
        }, 5000);
      }
    }
  }

  const api = { frame8n1, waveformPath, drawDividers, pageFromHash, showPage, rssiStep, rssiColor, staSignal, shouldPollStatus, togglePassword, addPasswordToggles,
    OPT_FIELDS, OPT_SELECTS, optDefaults, optRebootRequired, optFillForm, optReadForm, optStatus, sdioStatus, optLoad, optSave, optClearEmergency,
    PROTO_BITS, protoMaskFromChecks, protoSetChecks, protoFilterForBand, protoLabel, protoHint, radioFillForm, radioReadForm,
    radioRebootRequired, cameraUsable, cameraStatusText, cameraStorageText, cameraResultText, cameraPhotoRows,
    cameraLoad, cameraCapture, cameraDelete, cameraVisible, cameraFrame, cameraPreview,
    cameraSettingsFill, cameraSettingsLoad, cameraSettingsSave, cameraSettingsReset, cameraLiveChanged,
    cameraLiveEnabled, cameraOpen, parseWifiQr, wifiQrRead, wifiQrParse, wifiQrFill, wifiQrClear };
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
