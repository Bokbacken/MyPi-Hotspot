# MyPi-Hotspot — next steps and active visions

This document separates current Hotspot work from ideas that need independent design,
hardware verification, or a separate project. Research items are not implemented
features or performance guarantees.

## MyPi product family

- Keep the μ/π identity and project-specific serial divider as shared design concepts.
  The LCD uses the `H` character as 8N1 (start bit, eight data bits LSB-first, one stop
  bit); web pages encode `Hotspot` the same way. No Companion artwork is copied here.
- Use a character wheel on small touch displays. Prefer a full keyboard only when the
  available display area makes it practical.
- The Wi-Fi credential wheel has seven UTF-8 character ranges. Horizontal swipes change
  ranges; vertical swipes move the selection live, with password text masked by default.
- Keep the normal display, OTA progress, and emergency/rescue screens distinct. OTA
  and rescue continue to take priority over ordinary navigation and input.

## Hotspot UI direction

Current web views are Overview, Wi-Fi, Access point, Ethernet, Traffic, System / display,
Diagnostics, Update, C5 update, and Pins. The LCD Wi-Fi wheel can create a new STA
profile; the browser remains the place for advanced profile, radio, Ethernet, and
diagnostic settings.

Future refinements can bring traffic, clients, Ethernet state, and system information
into a more consistent LCD menu without making navigation stop the AP. Stopping the AP
must remain an explicit action.

## Dual-C5 research

Investigate a P4 with two C5 radios as a separate prototype:

- Verify the target board schematic and available P4 SDMMC slots/pins before changing
  the current single-C5 wiring or firmware.
- Evaluate independent radios for a Hotspot WAN/STA link and LAN/AP link. Keep reusable
  radio transport/control concepts separate from product policy rather than assuming
  that ESP-Hosted already provides a multi-slave API.
- Do not rely on 80 MHz Wi-Fi channels for C5. The potential benefit is separate radio
  resources and independent channel choice; measure real throughput before setting a
  target.

## Industrial AGV client and Layer-2 transport

A possible future project is an Ethernet-connected industrial client that maintains
connectivity while roaming. Its goal is make-before-break: keep the current radio path
active until a second STA link is established, then switch the active path. Define
measurable continuity targets (packet loss, interruption, latency, and session survival)
before selecting an implementation.

Multiple Ethernet-attached devices may require a Layer-2 tunnel to a fixed peer rather
than ordinary routing/NAT. Investigate addressing and identity continuity, tunnel
security, MTU, packet ordering/duplication, failure recovery, and the infrastructure
endpoint as a dedicated architecture effort. This is not a Hotspot feature commitment.

## Companion recovery idea

A Companion emergency/recovery mode may be useful, but should be Wi-Fi-oriented for
that product and designed separately from Hotspot's Ethernet rescue behavior.
