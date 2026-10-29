// SPDX-License-Identifier: GPL-3.0-or-later
// olv-cam: camera -> H.265 for the air unit, on the RV1103 (Rockchip MPI).
//
//   sensor (MIPI CSI-2) -> ISP with rkaiq 3A (the sensor's IQ file)
//   -> VI channel at the output size (the ISP scales down)
//   -> VENC H.265: hard CBR, column intra refresh, no B-frames, slices of at
//      most --slice-bytes (one FEC block each)
//   -> IVF stream (32-byte header, then 12-byte frame headers) on stdout.
//
// air starts it with the link's settings (air --source camera). It also runs
// on its own for bench checks:  olv-cam --seconds 5 --out /tmp/cam.ivf
// Supported sensors and how to wire them: air/src/cam_sensors.c, docs/camera.md.
#define _GNU_SOURCE // setenv, strdup

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rk_aiq_user_api2_sysctl.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"

#include "cam_sensors.h"
#include "olv_util.h"

#define CAM_ID 0 // first (only) camera: VI dev 0, pipe 0, ISP mainpath = chn 0
#define VI_CHN 0
#define VENC_CHN 0

static volatile sig_atomic_t stop, want_idr;

static void on_stop(int s) {
    (void)s;
    stop = 1;
}

static void on_idr(int s) {
    (void)s;
    want_idr = 1;
}

static uint64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t r = write(fd, b, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1; // EPIPE: the reader (air) has gone
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage: olv-cam [options]\n"
            "  --sensor auto|NAME   (default auto: the one whose driver found its chip)\n"
            "  --list               print the supported sensors and which is fitted\n"
            "  --size WxH           output picture (default 1280x720; the ISP scales down)\n"
            "  --fps N              (default 30, at most the sensor's)\n"
            "  --kbps N             H.265 CBR target (default 800)\n"
            "  --refresh S          column intra refresh sweep period (default 0.5)\n"
            "  --refresh-cols N     CTU columns refreshed per frame (default: from --refresh)\n"
            "  --slice-bytes N      max bytes per slice (default 0: one slice per picture)\n"
            "  --qp-min N --qp-max N  QP range (default 20..51)\n"
            "  --mirror --flip      mirror / flip the picture\n"
            "  --iq-dir DIR         rkaiq IQ files (default /oem/usr/share/iqfiles)\n"
            "  --out FILE|-         IVF output (default -: stdout)\n"
            "  --seconds S          stop after S seconds\n"
            "  --sysfs DIR          sysfs root for detection (default /sys)\n");
}

static void list_sensors(const char *sysfs) {
    const struct cam_sensor *found = cam_detect(sysfs);
    printf("sensor   output       IQ file                            module\n");
    for (size_t i = 0; i < CAM_N_SENSORS; i++) {
        const struct cam_sensor *s = &CAM_SENSORS[i];
        printf("%-8s %4dx%-4d@%-2d %-34s %s%s\n", s->name, s->w, s->h, s->fps, s->iq, s->module,
               s == found ? "   <- fitted" : "");
    }
    if (!found)
        printf("no camera found (no sensor driver bound under %s/bus/i2c/drivers)\n", sysfs);
}

int main(int argc, char **argv) {
    const char *sensor_name = "auto", *iq_dir = "/oem/usr/share/iqfiles", *out = "-",
               *sysfs = "/sys";
    int w = 1280, h = 720, fps = 30, kbps = 800, refresh_cols = 0, slice_bytes = 0, qp_min = 20,
        qp_max = 51;
    double refresh = 0.5, seconds = 0;
    bool list = false, mirror = false, flip = false;
    enum {
        O_SENSOR = 256,
        O_LIST,
        O_SIZE,
        O_FPS,
        O_KBPS,
        O_REFRESH,
        O_COLS,
        O_SLICE,
        O_QPMIN,
        O_QPMAX,
        O_MIRROR,
        O_FLIP,
        O_IQ,
        O_OUT,
        O_SEC,
        O_SYSFS
    };
    static const struct option lo[] = {{"sensor", 1, 0, O_SENSOR},
                                       {"list", 0, 0, O_LIST},
                                       {"size", 1, 0, O_SIZE},
                                       {"fps", 1, 0, O_FPS},
                                       {"kbps", 1, 0, O_KBPS},
                                       {"refresh", 1, 0, O_REFRESH},
                                       {"refresh-cols", 1, 0, O_COLS},
                                       {"slice-bytes", 1, 0, O_SLICE},
                                       {"qp-min", 1, 0, O_QPMIN},
                                       {"qp-max", 1, 0, O_QPMAX},
                                       {"mirror", 0, 0, O_MIRROR},
                                       {"flip", 0, 0, O_FLIP},
                                       {"iq-dir", 1, 0, O_IQ},
                                       {"out", 1, 0, O_OUT},
                                       {"seconds", 1, 0, O_SEC},
                                       {"sysfs", 1, 0, O_SYSFS},
                                       {"help", 0, 0, 'h'},
                                       {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
        switch (c) {
        case O_SENSOR:
            sensor_name = optarg;
            break;
        case O_LIST:
            list = true;
            break;
        case O_SIZE:
            if (sscanf(optarg, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
                fprintf(stderr, "olv-cam: --size wants WxH\n");
                return 2;
            }
            break;
        case O_FPS:
            fps = atoi(optarg);
            break;
        case O_KBPS:
            kbps = atoi(optarg);
            break;
        case O_REFRESH:
            refresh = atof(optarg);
            break;
        case O_COLS:
            refresh_cols = atoi(optarg);
            break;
        case O_SLICE:
            slice_bytes = atoi(optarg);
            break;
        case O_QPMIN:
            qp_min = atoi(optarg);
            break;
        case O_QPMAX:
            qp_max = atoi(optarg);
            break;
        case O_MIRROR:
            mirror = true;
            break;
        case O_FLIP:
            flip = true;
            break;
        case O_IQ:
            iq_dir = optarg;
            break;
        case O_OUT:
            out = optarg;
            break;
        case O_SEC:
            seconds = atof(optarg);
            break;
        case O_SYSFS:
            sysfs = optarg;
            break;
        default:
            usage();
            return c == 'h' ? 0 : 2;
        }
    }
    if (list) {
        list_sensors(sysfs);
        return 0;
    }

    const struct cam_sensor *sn =
        strcmp(sensor_name, "auto") ? cam_sensor_find(sensor_name) : cam_detect(sysfs);
    if (!sn) {
        if (strcmp(sensor_name, "auto"))
            fprintf(stderr, "olv-cam: unknown sensor %s (olv-cam --list)\n", sensor_name);
        else
            fprintf(stderr,
                    "olv-cam: no camera found: no sensor driver bound under "
                    "%s/bus/i2c/drivers (ribbon cable seated? dmesg | grep -i sc)\n",
                    sysfs);
        return 2;
    }
    int want_w = w, want_h = h, want_fps = fps;
    cam_fit_size(sn, &w, &h);
    fps = cam_fit_fps(sn, fps);
    if (w != want_w || h != want_h || fps != want_fps)
        fprintf(stderr, "olv-cam: %s gives %dx%d@%d (asked %dx%d@%d)\n", sn->name, w, h, fps,
                want_w, want_h, want_fps);
    if (!refresh_cols)
        refresh_cols = cam_refresh_cols(w, fps, refresh);
    char iq_path[512];
    snprintf(iq_path, sizeof(iq_path), "%s/%s.json", iq_dir, sn->iq);
    if (access(iq_path, R_OK) != 0)
        fprintf(stderr, "olv-cam: WARNING: %s missing; rkaiq will look for another IQ file\n",
                iq_path);
    int fd = strcmp(out, "-") ? -1 : STDOUT_FILENO;
    FILE *outf = NULL;
    if (fd < 0) {
        if (!(outf = fopen(out, "wb"))) {
            perror(out);
            return 1;
        }
        fd = fileno(outf);
    }
    fprintf(stderr,
            "olv-cam: %s -> %dx%d@%d H.265 CBR %d kbit/s, column refresh %d CTU/frame (%.2f s), "
            "slices <= %d B, QP %d..%d\n",
            sn->name, w, h, fps, kbps, refresh_cols, (double)((w + 63) / 64) / refresh_cols / fps,
            slice_bytes, qp_min, qp_max);

    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    signal(SIGUSR1, on_idr);
    signal(SIGPIPE, SIG_IGN);

    int ret = 1;
    // ---- ISP 3A (auto exposure, white balance, ...) with the sensor's IQ file
    setenv("HDR_MODE", "0", 1); // linear; read by rkaiq at init
    rk_aiq_static_info_t info;
    memset(&info, 0, sizeof(info));
    if (rk_aiq_uapi2_sysctl_enumStaticMetas(CAM_ID, &info) != XCAM_RETURN_NO_ERROR) {
        fprintf(stderr, "olv-cam: rkaiq finds no camera (ISP driver loaded? /proc/rkisp*)\n");
        return 3;
    }
    rk_aiq_sys_ctx_t *aiq =
        rk_aiq_uapi2_sysctl_init(info.sensor_info.sensor_name, iq_dir, NULL, NULL);
    if (!aiq) {
        fprintf(stderr, "olv-cam: rkaiq init failed for %s (IQ files in %s?)\n",
                info.sensor_info.sensor_name, iq_dir);
        return 3;
    }
    if (rk_aiq_uapi2_sysctl_prepare(aiq, 0, 0, RK_AIQ_WORKING_MODE_NORMAL) !=
            XCAM_RETURN_NO_ERROR ||
        rk_aiq_uapi2_sysctl_start(aiq) != XCAM_RETURN_NO_ERROR) {
        fprintf(stderr, "olv-cam: rkaiq prepare/start failed\n");
        rk_aiq_uapi2_sysctl_deinit(aiq);
        return 3;
    }
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        fprintf(stderr, "olv-cam: RK_MPI_SYS_Init failed (is rkipc still running?)\n");
        goto out_aiq;
    }

    // ---- VI: device 0 -> pipe 0 -> channel 0 (ISP main path, scaled)
    VI_DEV_ATTR_S dev;
    memset(&dev, 0, sizeof(dev));
    if (RK_MPI_VI_GetDevAttr(CAM_ID, &dev) == RK_ERR_VI_NOT_CONFIG &&
        RK_MPI_VI_SetDevAttr(CAM_ID, &dev) != RK_SUCCESS) {
        fprintf(stderr, "olv-cam: VI SetDevAttr failed\n");
        goto out_sys;
    }
    if (RK_MPI_VI_GetDevIsEnable(CAM_ID) != RK_SUCCESS) {
        VI_DEV_BIND_PIPE_S bind;
        memset(&bind, 0, sizeof(bind));
        bind.u32Num = 1;
        bind.PipeId[0] = CAM_ID;
        if (RK_MPI_VI_EnableDev(CAM_ID) != RK_SUCCESS ||
            RK_MPI_VI_SetDevBindPipe(CAM_ID, &bind) != RK_SUCCESS) {
            fprintf(stderr, "olv-cam: VI EnableDev/BindPipe failed\n");
            goto out_sys;
        }
    }
    VI_CHN_ATTR_S vi;
    memset(&vi, 0, sizeof(vi));
    vi.stIspOpt.u32BufCount = 2;
    vi.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    vi.stSize.u32Width = (RK_U32)w;
    vi.stSize.u32Height = (RK_U32)h;
    vi.enPixelFormat = RK_FMT_YUV420SP;
    vi.enCompressMode = COMPRESS_MODE_NONE;
    vi.bMirror = mirror ? RK_TRUE : RK_FALSE;
    vi.bFlip = flip ? RK_TRUE : RK_FALSE;
    vi.u32Depth = 0; // frames go to VENC through the bind, not to us
    if (RK_MPI_VI_SetChnAttr(CAM_ID, VI_CHN, &vi) != RK_SUCCESS ||
        RK_MPI_VI_EnableChn(CAM_ID, VI_CHN) != RK_SUCCESS) {
        fprintf(stderr, "olv-cam: VI channel %dx%d failed\n", w, h);
        goto out_dev;
    }

    // ---- VENC: H.265 hard CBR (1 s window, the shortest), P-only
    VENC_CHN_ATTR_S ve;
    memset(&ve, 0, sizeof(ve));
    ve.stVencAttr.enType = RK_VIDEO_ID_HEVC;
    ve.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    ve.stVencAttr.u32Profile = H265E_PROFILE_MAIN;
    ve.stVencAttr.u32PicWidth = (RK_U32)w;
    ve.stVencAttr.u32PicHeight = (RK_U32)h;
    ve.stVencAttr.u32VirWidth = (RK_U32)w;
    ve.stVencAttr.u32VirHeight = (RK_U32)h;
    ve.stVencAttr.u32StreamBufCnt = 2;
    ve.stVencAttr.u32BufSize = (RK_U32)(w * h / 2);
    ve.stVencAttr.bByFrame = RK_TRUE; // one pack per picture, all its slices
    ve.stVencAttr.enMirror = MIRROR_NONE;
    ve.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
    // "Infinite" GOP: one IDR at start; column intra refresh does the rest.
    // 65536 is the API maximum (36 min at 30 fps).
    ve.stRcAttr.stH265Cbr.u32Gop = 65536;
    ve.stRcAttr.stH265Cbr.u32SrcFrameRateNum = (RK_U32)fps;
    ve.stRcAttr.stH265Cbr.u32SrcFrameRateDen = 1;
    ve.stRcAttr.stH265Cbr.fr32DstFrameRateNum = (RK_U32)fps;
    ve.stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
    ve.stRcAttr.stH265Cbr.u32BitRate = (RK_U32)kbps;
    ve.stRcAttr.stH265Cbr.u32StatTime = 1;
    ve.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
    if (RK_MPI_VENC_CreateChn(VENC_CHN, &ve) != RK_SUCCESS) {
        fprintf(stderr, "olv-cam: VENC CreateChn failed\n");
        goto out_vi;
    }
    VENC_RC_PARAM_S rc;
    if (RK_MPI_VENC_GetRcParam(VENC_CHN, &rc) == RK_SUCCESS) {
        rc.stParamH265.u32MinQp = rc.stParamH265.u32MinIQp = (RK_U32)qp_min;
        rc.stParamH265.u32MaxQp = rc.stParamH265.u32MaxIQp = (RK_U32)qp_max;
        rc.stParamH265.u32FrmMinQp = rc.stParamH265.u32FrmMinIQp = (RK_U32)qp_min;
        rc.stParamH265.u32FrmMaxQp = rc.stParamH265.u32FrmMaxIQp = (RK_U32)qp_max;
        if (RK_MPI_VENC_SetRcParam(VENC_CHN, &rc) != RK_SUCCESS)
            fprintf(stderr, "olv-cam: WARNING: QP range not accepted\n");
    }
    VENC_INTRA_REFRESH_S ir;
    memset(&ir, 0, sizeof(ir));
    RK_MPI_VENC_GetIntraRefresh(VENC_CHN, &ir);
    ir.bRefreshEnable = RK_TRUE;
    ir.enIntraRefreshMode = INTRA_REFRESH_COLUMN;
    ir.u32RefreshNum = (RK_U32)refresh_cols;
    if (RK_MPI_VENC_SetIntraRefresh(VENC_CHN, &ir) != RK_SUCCESS)
        fprintf(stderr, "olv-cam: WARNING: column intra refresh not accepted; a lost packet "
                        "leaves damage until the next IDR\n");
    if (slice_bytes > 0) {
        VENC_SLICE_SPLIT_S sl;
        memset(&sl, 0, sizeof(sl));
        sl.bSplitEnable = RK_TRUE;
        sl.u32SplitMode = 0; // by bytes
        sl.u32SplitSize = (RK_U32)slice_bytes;
        if (RK_MPI_VENC_SetSliceSplit(VENC_CHN, &sl) != RK_SUCCESS)
            fprintf(stderr, "olv-cam: WARNING: slice split not accepted\n");
    }
    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = -1;
    if (RK_MPI_VENC_StartRecvFrame(VENC_CHN, &recv) != RK_SUCCESS) {
        fprintf(stderr, "olv-cam: VENC StartRecvFrame failed\n");
        goto out_venc;
    }
    MPP_CHN_S src = {.enModId = RK_ID_VI, .s32DevId = CAM_ID, .s32ChnId = VI_CHN};
    MPP_CHN_S dst = {.enModId = RK_ID_VENC, .s32DevId = 0, .s32ChnId = VENC_CHN};
    if (RK_MPI_SYS_Bind(&src, &dst) != RK_SUCCESS) {
        fprintf(stderr, "olv-cam: bind VI -> VENC failed\n");
        goto out_recv;
    }

    // ---- stream out
    uint8_t hdr[32] = {'D', 'K', 'I', 'F'};
    olv_put_u16(hdr + 6, 32);
    memcpy(hdr + 8, "H265", 4);
    olv_put_u16(hdr + 12, (uint16_t)w);
    olv_put_u16(hdr + 14, (uint16_t)h);
    olv_put_u32(hdr + 16, (uint32_t)fps);
    olv_put_u32(hdr + 20, 1);
    if (write_all(fd, hdr, sizeof(hdr)) != 0)
        goto out_bind;
    VENC_PACK_S pack;
    VENC_STREAM_S st;
    memset(&st, 0, sizeof(st));
    st.pstPack = &pack;
    uint64_t t0 = mono_us(), last = t0, report = t0 + 5000000u;
    uint32_t frames = 0;
    uint64_t bytes = 0;
    ret = 0;
    while (!stop) {
        uint64_t now = mono_us();
        if (seconds > 0 && now - t0 >= (uint64_t)(seconds * 1e6))
            break;
        if (now - last > 2000000u) {
            fprintf(stderr, "olv-cam: no picture for 2 s; giving up\n");
            ret = 4;
            break;
        }
        if (want_idr) {
            want_idr = 0;
            RK_MPI_VENC_RequestIDR(VENC_CHN, RK_TRUE);
        }
        if (RK_MPI_VENC_GetStream(VENC_CHN, &st, 100) != RK_SUCCESS)
            continue;
        const void *data = RK_MPI_MB_Handle2VirAddr(pack.pMbBlk);
        uint8_t fh[12];
        olv_put_u32(fh, pack.u32Len);
        olv_put_u32(fh + 4, (uint32_t)pack.u64PTS);
        olv_put_u32(fh + 8, (uint32_t)(pack.u64PTS >> 32));
        bool ok =
            data && write_all(fd, fh, sizeof(fh)) == 0 && write_all(fd, data, pack.u32Len) == 0;
        bytes += pack.u32Len;
        RK_MPI_VENC_ReleaseStream(VENC_CHN, &st);
        if (!ok)
            break; // the reader has gone: stop quietly
        frames++;
        last = now;
        if (now >= report) {
            fprintf(stderr, "olv-cam: %u frames, %.0f kbit/s\n", frames,
                    bytes * 8 / 1000.0 / ((now - t0) / 1e6));
            report += 5000000u;
        }
    }

out_bind:
    RK_MPI_SYS_UnBind(&src, &dst);
out_recv:
    RK_MPI_VENC_StopRecvFrame(VENC_CHN);
out_venc:
    RK_MPI_VENC_DestroyChn(VENC_CHN);
out_vi:
    RK_MPI_VI_DisableChn(CAM_ID, VI_CHN);
out_dev:
    RK_MPI_VI_DisableDev(CAM_ID);
out_sys:
    RK_MPI_SYS_Exit();
out_aiq:
    rk_aiq_uapi2_sysctl_stop(aiq, false);
    rk_aiq_uapi2_sysctl_deinit(aiq);
    if (outf)
        fclose(outf);
    return ret;
}
