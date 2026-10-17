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

  const api = { frame8n1, waveformPath, drawDividers };
  root.MyPiSerial = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  if (root.document) {
    if (root.document.readyState === "loading") {
      root.document.addEventListener("DOMContentLoaded", () => drawDividers(root.document), { once: true });
    } else {
      drawDividers(root.document);
    }
  }
})(globalThis);
