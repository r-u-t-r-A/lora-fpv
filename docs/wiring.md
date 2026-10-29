# Wiring: Luckfox Pico (RV1103) to the radio module

Two supported modules: the NiceRF LoRa2021F33-2G4 (LR2021, both bands) and
the Ebyte E28-2G4M27S (SX1280, 2.4 GHz only, see the second table).

## NiceRF LoRa2021F33-2G4 (LR2021)

Same wiring on the air unit and on every VRX. **Pin numbers are not decided
yet**: fill this table from the Luckfox Pico Mini pinout and the NiceRF
datasheet, then pass the GPIO numbers to `flrc_bench` (and later to
`air`/`vrxd` config).

GPIO numbers follow the Luckfox convention: `bank * 32 + group * 8 + n`,
with group A=0, B=1, C=2, D=3. Example: GPIO1_C7 = 32 + 16 + 7 = 55, which is
line 23 of `/dev/gpiochip1`.

| Signal | LR2021 / module pin | Luckfox pin | GPIO number | Notes |
|---|---|---|---|---|
| SPI SCK | SCK | SPI0 CLK | (spidev) | `luckfox-config`: enable SPI0 |
| SPI MOSI | MOSI | SPI0 MOSI | (spidev) | |
| SPI MISO | MISO | SPI0 MISO | (spidev) | |
| SPI CS | NSS | SPI0 CS0 | (spidev) | driven by spidev, not by RadioLib |
| BUSY | BUSY | | | input |
| IRQ | DIO5 | | | input, rising edge (RadioLib uses DIO5 as IRQ) |
| NRESET | NRESET | | | output |
| PA/LNA enable(s) | per NiceRF datasheet | | | `--rf-pins` / `--rf-table` |
| TCXO | DIO3 (if TCXO) | n/a | | `--tcxo-mv` (0 for a crystal) |
| VCC (PA) | per datasheet | own supply | | a 1 W PA draws far more than the Luckfox 3.3 V rail can give |
| GND | GND | GND | | short, common ground |

Before powering the PA: antenna or a 50-ohm load (or the attenuator chain)
on **both** RF ports in use. Never transmit into an open port.

## Ebyte E28-2G4M27S (SX1280 + PA/LNA)

Same SPI and GPIO scheme; the SX1280 IRQ is DIO1, and the PA/LNA are
switched by the host on TXEN and RXEN (high = that path on; never both
high, the backend goes through standby between RX and TX). The E28 has a
crystal, no TCXO. The module pin numbers are in the Ebyte datasheet.

| Signal | E28 pin | Luckfox pin | GPIO number | Notes |
|---|---|---|---|---|
| SPI SCK/MOSI/MISO | SCK, MOSI, MISO | SPI0 | (spidev) | |
| SPI CS | NSS | SPI0 CS0 | (spidev) | |
| BUSY | BUSY | | | input |
| IRQ | DIO1 | | | input, rising edge |
| NRESET | NRST | | | output |
| TXEN | TXEN | | | output, `rf_pins` first entry |
| RXEN | RXEN | | | output, `rf_pins` second entry |
| DIO2, DIO3 | | n/c | | unused |
| VCC | VCC | own 3.3 V supply | | the PA draws far more in TX than the Luckfox rail gives; check the datasheet |
| GND | GND | GND | | |

`/etc/olv-radio.conf` for this module (fill in the GPIO numbers):

```
module=e28-2g4m27s     # chip=sx1280, rf_table, PA 25 dB, LNA 12 dB, max -1 dBm, power -18
spidev=/dev/spidev0.0
busy=
irq=                   # DIO1
nreset=
rf_pins=TXEN,RXEN      # GPIO numbers, in this order
```

On the VRXs, `/etc/vrx.conf` can stay at `band=auto mode=auto`: with an
SX1280 the only mode left is hq1300.
