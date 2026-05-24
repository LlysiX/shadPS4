# Community kit definitions

Drop user-contributed TOMLs in here (or any subfolder). The runtime
loader recursively scans `scripts/kits/` and `<UserDir>/kits/`.

## Naming

`<vendor>_<model>_<vid>_<pid>.toml` for HID kits with a fixed VID:PID.
For XInput name-matched kits (no fixed VID:PID), use a descriptive name
like `gh_xbox360_guitar.toml`.

## Source spec

Layouts in this directory are derived from
[PlasticBand](https://github.com/TheNathannator/PlasticBand). Cite the
exact spec file in a comment at the top of each TOML so future
maintainers can re-verify against upstream changes.

## What's currently shipped

| Kit | Source | Notes |
|---|---|---|
| `pdp_riffmaster_ps5_0e6f_0249.toml` | HID | PDP RiffMaster, PS5 mode |
| `gh_xbox360_guitar.toml` | XInput | Any GH-series X360 guitar (name-matched) |
| `gh_xbox360_drums.toml` | XInput | Any GH-series X360 drum kit (name-matched) |

## Not supported (yet)

- **Xbox One / Series instruments** (PDP Jaguar XB1, MadCatz Drumkit XB1,
  Riffmaster XB1, etc.). They use the GIPUSB protocol — SDL_GameController
  exposes the buttons but not the custom velocity packet. Velocity-aware
  support would require raw USB packet parsing (see
  [RB4InstrumentMapper](https://github.com/TheNathannator/RB4InstrumentMapper)
  for a reference Windows implementation).
- **Pro Guitar / Keyboard / Turntable** — niche RB peripherals with
  unique protocols; no plans to add unless someone asks.
- **6-fret Guitar Hero Live** — different fret enumeration; RB4 doesn't
  use it anyway.
