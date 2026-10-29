# Wiring: Luckfox Pico (RV1103) to NiceRF LoRa2021F33-2G4

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
