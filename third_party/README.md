# third_party

| Library | How | Licence | Used by |
|---|---|---|---|
| [zfex](https://github.com/WojciechMigda/zfex) | vendored C sources in `zfex/` (commit in `zfex/UPSTREAM_COMMIT`) | GPL-2.0-or-later (or TGPPL) | `common/fec.c`, web WASM |
| [RadioLib](https://github.com/jgromes/RadioLib) | git submodule `RadioLib/`, pinned to tag 7.8.1 | MIT | `radio/` (LR2021 backend) |
| [libde265](https://github.com/strukturag/libde265) | git submodule `libde265/` | LGPL-3.0 | web software-decode fallback (WASM) |

Only the C files of zfex are vendored (`zfex.c`, `*.h`); the Python and
Haskell bindings are not. Do not edit them in place: if a patch is needed,
keep it in `patches/` and note it here.

After cloning: `git submodule update --init third_party/RadioLib` (needed for
`radio/` and `tools/`; configure with `-DOLV_RADIO=OFF` to build without it).
libde265 is not added yet (WASM software-decode fallback, later).
