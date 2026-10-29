// SPDX-License-Identifier: GPL-3.0-or-later
#include "linux_hal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

enum { HAL_INPUT = 0, HAL_OUTPUT = 1, HAL_LOW = 0, HAL_HIGH = 1, HAL_RISING = 1, HAL_FALLING = 2 };
enum { MODE_EDGE = 2 };

LinuxHal::LinuxHal(const char *spidev, uint32_t spi_hz)
    : RadioLibHal(HAL_INPUT, HAL_OUTPUT, HAL_LOW, HAL_HIGH, HAL_RISING, HAL_FALLING),
      spidev_(spidev), spi_hz_(spi_hz) {
    for (int &fd : chip_fd_)
        fd = -1;
    t0_ns_ = monotonicNs();
}

LinuxHal::~LinuxHal() {
    term();
}

uint64_t LinuxHal::monotonicNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void LinuxHal::init() {
    spiBegin();
}

void LinuxHal::term() {
    spiEnd();
    for (int i = 0; i < n_lines_; i++)
        if (lines_[i].fd >= 0)
            close(lines_[i].fd);
    n_lines_ = 0;
    irq_ = nullptr;
    for (int &fd : chip_fd_)
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
}

LinuxHal::Line *LinuxHal::find(uint32_t pin) {
    for (int i = 0; i < n_lines_; i++)
        if (lines_[i].pin == pin)
            return &lines_[i];
    return nullptr;
}

LinuxHal::Line *LinuxHal::request(uint32_t pin, uint32_t mode, uint32_t initial) {
    uint32_t chip = pin / 32, offset = pin % 32;
    if (chip >= 8) {
        fprintf(stderr, "hal: GPIO %u out of range\n", pin);
        failed_ = true;
        return nullptr;
    }
    if (chip_fd_[chip] < 0) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/gpiochip%u", chip);
        chip_fd_[chip] = open(path, O_RDWR | O_CLOEXEC);
        if (chip_fd_[chip] < 0) {
            fprintf(stderr, "hal: %s: %s\n", path, strerror(errno));
            failed_ = true;
            return nullptr;
        }
    }
    Line *l = find(pin);
    if (l && l->fd >= 0) {
        close(l->fd);
        l->fd = -1;
    }
    if (!l) {
        if (n_lines_ == MAX_LINES) {
            failed_ = true;
            return nullptr;
        }
        l = &lines_[n_lines_++];
        l->pin = pin;
    }
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = offset;
    req.num_lines = 1;
    snprintf(req.consumer, sizeof(req.consumer), "olv-lr2021");
    if (mode == HAL_OUTPUT) {
        req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
        req.config.num_attrs = 1;
        req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        req.config.attrs[0].attr.values = initial ? 1 : 0;
        req.config.attrs[0].mask = 1;
    } else {
        req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
        if (mode == MODE_EDGE)
            req.config.flags |= GPIO_V2_LINE_FLAG_EDGE_RISING;
    }
    if (ioctl(chip_fd_[chip], GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        fprintf(stderr, "hal: request GPIO %u (chip %u line %u): %s\n", pin, chip, offset,
                strerror(errno));
        failed_ = true;
        l->fd = -1;
        return nullptr;
    }
    l->fd = req.fd;
    l->mode = mode;
    return l;
}

void LinuxHal::pinMode(uint32_t pin, uint32_t mode) {
    if (pin == RADIOLIB_NC)
        return;
    Line *l = find(pin);
    // Keep an armed IRQ line as it is: RadioLib re-declares it as an input.
    if (l && l->fd >= 0 && (l->mode == mode || (l->mode == MODE_EDGE && mode == HAL_INPUT)))
        return;
    request(pin, mode, HAL_HIGH);
}

void LinuxHal::digitalWrite(uint32_t pin, uint32_t value) {
    if (pin == RADIOLIB_NC)
        return;
    Line *l = find(pin);
    if (!l || l->fd < 0 || l->mode != HAL_OUTPUT)
        l = request(pin, HAL_OUTPUT, value);
    if (!l)
        return;
    struct gpio_v2_line_values v = {value ? 1ull : 0ull, 1};
    if (ioctl(l->fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0)
        fprintf(stderr, "hal: write GPIO %u: %s\n", pin, strerror(errno));
}

uint32_t LinuxHal::digitalRead(uint32_t pin) {
    if (pin == RADIOLIB_NC)
        return 0;
    Line *l = find(pin);
    if (!l || l->fd < 0)
        l = request(pin, HAL_INPUT, 0);
    if (!l)
        return 0;
    struct gpio_v2_line_values v = {0, 1};
    if (ioctl(l->fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0)
        return 0;
    return (uint32_t)(v.bits & 1);
}

// RadioLib's interrupt callbacks are not used: radio_poll() waits on the edge
// events instead (see armIrq/waitIrq).
void LinuxHal::attachInterrupt(uint32_t, void (*)(void), uint32_t) {}
void LinuxHal::detachInterrupt(uint32_t) {}

bool LinuxHal::armIrq(uint32_t pin) {
    irq_ = request(pin, MODE_EDGE, 0);
    return irq_ != nullptr;
}

int LinuxHal::waitIrq(uint32_t timeout_us, uint64_t *t_ns) {
    if (!irq_ || irq_->fd < 0)
        return -1;
    struct pollfd pf = {irq_->fd, POLLIN, 0};
    int r = poll(&pf, 1, 0);
    if (r == 0 && digitalRead(irq_->pin)) {
        // Line already high: the edge was consumed before the flags were read.
        *t_ns = monotonicNs();
        return 1;
    }
    if (r == 0) {
        struct timespec ts = {(time_t)(timeout_us / 1000000u),
                              (long)(timeout_us % 1000000u) * 1000};
        r = ppoll(&pf, 1, &ts, nullptr);
    }
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (r == 0)
        return 0;
    struct gpio_v2_line_event ev[4];
    ssize_t n = read(irq_->fd, ev, sizeof(ev));
    if (n < (ssize_t)sizeof(ev[0]))
        return -1;
    // Several queued edges: the last one is the most recent IRQ.
    *t_ns = ev[n / (ssize_t)sizeof(ev[0]) - 1].timestamp_ns;
    return 1;
}

void LinuxHal::delay(RadioLibTime_t ms) {
    delayMicroseconds(ms * 1000u);
}

void LinuxHal::delayMicroseconds(RadioLibTime_t us) {
    struct timespec ts = {(time_t)(us / 1000000u), (long)(us % 1000000u) * 1000};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

RadioLibTime_t LinuxHal::millis() {
    return (RadioLibTime_t)((monotonicNs() - t0_ns_) / 1000000u);
}

RadioLibTime_t LinuxHal::micros() {
    return (RadioLibTime_t)((monotonicNs() - t0_ns_) / 1000u);
}

long LinuxHal::pulseIn(uint32_t, uint32_t, RadioLibTime_t) {
    return 0; // not used by the LR2021 driver
}

void LinuxHal::spiBegin() {
    if (spi_fd_ >= 0)
        return;
    spi_fd_ = open(spidev_, O_RDWR | O_CLOEXEC);
    if (spi_fd_ < 0) {
        fprintf(stderr, "hal: %s: %s\n", spidev_, strerror(errno));
        return;
    }
    uint8_t mode = SPI_MODE_0, bits = 8;
    if (ioctl(spi_fd_, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(spi_fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spi_fd_, SPI_IOC_WR_MAX_SPEED_HZ, &spi_hz_) < 0) {
        fprintf(stderr, "hal: configure %s: %s\n", spidev_, strerror(errno));
        failed_ = true;
    }
}

void LinuxHal::spiBeginTransaction() {}

void LinuxHal::spiTransfer(uint8_t *out, size_t len, uint8_t *in) {
    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (uintptr_t)out;
    tr.rx_buf = (uintptr_t)in;
    tr.len = (uint32_t)len;
    tr.speed_hz = spi_hz_;
    tr.bits_per_word = 8;
    if (ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &tr) < 0) {
        fprintf(stderr, "hal: SPI transfer: %s\n", strerror(errno));
        failed_ = true;
    }
}

void LinuxHal::spiEndTransaction() {}

void LinuxHal::spiEnd() {
    if (spi_fd_ >= 0) {
        close(spi_fd_);
        spi_fd_ = -1;
    }
}
