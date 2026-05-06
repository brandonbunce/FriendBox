# Product Specs Index

Product specs define user-facing behavior before implementation begins. Each spec should answer: what does the user experience, what are the edge cases, and what does "done" look like.

## Status legend

| Status | Meaning |
|---|---|
| Shipped | Implemented and on hardware |
| In progress | Active development |
| Specced | Spec written, not yet built |
| Planned | On roadmap, no spec yet |

---

## Feature registry

| Feature | Status | Spec |
|---|---|---|
| Drawing canvas | Shipped | — |
| Drawing menu GUI | Shipped | — |
| Save slots (SD card) | Shipped | — |
| Drawing tools (pencil, fill, dither) | Shipped | — |
| Hall effect menu sensor | Shipped | — |
| Send drawing to friend | Planned | — |
| Receive drawing from friend | Planned | — |
| Friend list | Planned | — |
| Home GUI | Planned | — |
| Settings GUI | Planned | — |
| Registration GUI | Planned | — |
| Memories / send history | Planned | — |
| Stickers | Planned | — |
| Rotate gestures | Planned | — |
| OTA firmware updates | Planned | — |
| Hall effect lid sensor | Planned | — |
| LED array | Planned | — |
| Speaker / audio | Planned | — |

---

## Adding a spec

Create a new `.md` file in this directory. Add a row to the table above with a link. Spec files should include:
- **User story**: who does what and why
- **UI flow**: screen-by-screen walkthrough
- **API contract** (if network involved): request/response format
- **Edge cases**: error states, missing data, timeout
- **Done criteria**: how to verify it works on hardware
