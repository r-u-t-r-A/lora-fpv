// SPDX-License-Identifier: GPL-3.0-or-later
// RadioLib HAL for embedded Linux (Luckfox RV1103, or any Linux >= 5.10):
// SPI through spidev (hardware chip select), GPIO through the kernel GPIO
// character device (v2 uAPI), no libgpiod needed. Interrupts are not
// emulated with threads: the radio code polls the IRQ line's edge events,
// whose kernel timestamps give precise RX/TX done times.
#ifndef OLV_LINUX_HAL_H
#define OLV_LINUX_HAL_H

#include <RadioLib.h>

#include <stdint.h>

class LinuxHal : public RadioLibHal {
  public:
    enum { MAX_LINES = 8 };

    LinuxHal(const char *spidev, uint32_t spi_hz);
    ~LinuxHal() override;

    void init() override;
    void term() override;

    void pinMode(uint32_t pin, uint32_t mode) override;
    void digitalWrite(uint32_t pin, uint32_t value) override;
    uint32_t digitalRead(uint32_t pin) override;
    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override;
    void detachInterrupt(uint32_t interruptNum) override;
    void delay(RadioLibTime_t ms) override;
    void delayMicroseconds(RadioLibTime_t us) override;
    RadioLibTime_t millis() override;
    RadioLibTime_t micros() override;
    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override;
    void spiBegin() override;
    void spiBeginTransaction() override;
    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override;
    void spiEndTransaction() override;
    void spiEnd() override;

    bool ok() const { return spi_fd_ >= 0 && !failed_; }

    // Requests pin as an input with rising-edge events (the LR2021 IRQ line).
    bool armIrq(uint32_t pin);

    // Waits for a rising edge on the armed IRQ line. Returns 1 with the
    // event's CLOCK_MONOTONIC time in *t_ns, 0 on timeout, -1 on error. If
    // the line is already high (edge consumed earlier), returns 1 at once.
    int waitIrq(uint32_t timeout_us, uint64_t *t_ns);

    static uint64_t monotonicNs();

  private:
    struct Line {
        uint32_t pin;
        int fd;
        uint32_t mode; // 0 input, 1 output, 2 input + edge
    };

    Line *find(uint32_t pin);
    Line *request(uint32_t pin, uint32_t mode, uint32_t initial);

    const char *spidev_;
    uint32_t spi_hz_;
    int spi_fd_ = -1;
    bool failed_ = false;
    Line lines_[MAX_LINES];
    int n_lines_ = 0;
    int chip_fd_[8];
    Line *irq_ = nullptr;
    uint64_t t0_ns_;
};

#endif
