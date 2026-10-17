"use strict";

const assert = require("node:assert/strict");
const { frame8n1, waveformPath } = require("../main/web_ui.js");

assert.deepEqual(frame8n1("H"), [0, 0, 0, 0, 1, 0, 0, 1, 0, 1]);

const text = "Hotspot";
const allBits = frame8n1(text);
assert.equal(allBits.length, text.length * 10);
for (let i = 0; i < text.length; i++) {
  const byte = text.charCodeAt(i);
  assert.deepEqual(allBits.slice(i * 10, (i + 1) * 10), [
    0, ...Array.from({ length: 8 }, (_, bit) => (byte >> bit) & 1), 1,
  ]);
}

const waveform = waveformPath(frame8n1("H"), 7, 12, 23);
assert.equal(waveform.width, 70);
assert.equal(waveform.path.startsWith("M0 23"), true);
assert.equal(waveform.path.endsWith("H70"), true);
assert.equal((waveform.path.match(/ V/g) || []).length, 5);

console.log("Web UI serial tests passed");
