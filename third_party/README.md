# third_party

| Library | How | Licence | Used by |
|---|---|---|---|
| [zfex](https://github.com/WojciechMigda/zfex) | vendored C sources in `zfex/` (commit in `zfex/UPSTREAM_COMMIT`) | GPL-2.0-or-later (or TGPPL) | `common/fec.c`, web WASM |
| [RadioLib](https://github.com/jgromes/RadioLib) | git submodule `RadioLib/` | MIT | `firmware/modem` |
| [libde265](https://github.com/strukturag/libde265) | git submodule `libde265/` | LGPL-3.0 | web software-decode fallback (WASM) |

Only the C files of zfex are vendored (`zfex.c`, `*.h`); the Python and
Haskell bindings are not. Do not edit them in place: if a patch is needed,
keep it in `patches/` and note it here.

After cloning: `git submodule update --init --recursive` (only needed for the
firmware and the WASM decoder; host builds and tests do not need submodules).
