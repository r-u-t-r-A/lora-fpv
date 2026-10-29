#!/bin/sh
# Builds web/public/olv.wasm from the shared C reassembler + zfex.
# Needs Emscripten (emcc) on the PATH.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
mkdir -p "$here/../public"
# Warnings are checked by the host build (olv_wasm_check); zfex is upstream code.
emcc -O2 -DNDEBUG -std=c11 -w \
    -I"$root/common" -I"$root/third_party/zfex" \
    "$here/olv_wasm.c" "$root/common/vpkt.c" "$root/common/fec.c" \
    "$root/common/h265.c" "$root/common/ota.c" "$root/third_party/zfex/zfex.c" \
    -sSTANDALONE_WASM --no-entry -sINITIAL_MEMORY=16MB -sALLOW_MEMORY_GROWTH=1 \
    -sEXPORTED_FUNCTIONS=_olv_rx_init,_olv_in_buf,_olv_rx_push,_olv_rx_tick,_olv_rx_flush,_olv_frame_count,_olv_frame_data,_olv_frame_len,_olv_frame_info,_olv_frame_blocks,_olv_frame_pop,_olv_stats,_malloc,_free \
    -o "$here/../public/olv.wasm"
echo "wrote web/public/olv.wasm"
