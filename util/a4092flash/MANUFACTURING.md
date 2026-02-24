# A4092 Manufacturing Tool (mfgtool)

`mfgtool` is an AmigaOS CLI utility for programming, reading, and inspecting
manufacturing data stored in the A4092's SPI flash. It also includes all the
low-level SPI flash commands from the original `spiutil` (read, write, erase,
verify, patch, status, id).

## Flash Layout

The 512 KB W25X40 SPI flash is partitioned as follows:

| Address Range         | Size   | Contents            |
|-----------------------|--------|---------------------|
| `0x00000` - `0x7CFFF` | 500 KB | Firmware/ROM Image  |
| `0x7D000` - `0x7EFFF` |   8 KB | NVRAM/Settings      |
| `0x7F000` - `0x7FFFF` |   4 KB | Manufacturing Data  |

Manufacturing data occupies a single 4 KB sector at `0x7F000`. The tool uses
a 4 KB sector erase (opcode `0x20`) so that writing manufacturing data never
touches the NVRAM or firmware regions.

## Manufacturing Data Structure

The `struct mfg_data` is exactly 256 bytes and is defined in `mfg_flash.h`.
Integrity is checked by XORing all 64 uint32 words of the structure together;
the result must be zero for a valid record.

## Config File Format

Manufacturing data is described in a human-editable text file using
`key = value` pairs. Lines starting with `#` are comments. All fields are
optional; unset fields default to zero.

### Fields

| Key                  | Format         | Example        | Description                                |
|----------------------|----------------|----------------|--------------------------------------------|
| `card_type`          | string (max 7) | `A4092`        | Card model identifier                      |
| `serial`             | 8 digits       | `00000001`     | Serial number (see Serial Handling below)  |
| `hw_revision`        | `major.minor`  | `3.0`          | Hardware revision (stored as `0x0300`)     |
| `pcb_color`          | `#RGB` or name | `#080`         | PCB color in RGB444 hex or alias name      |
| `assembler_name`     | string (max 31)| `Stefan Reinauer` | Person who assembled the board          |
| `assembly_factory`   | string (max 31)| `JLCPCB`       | Assembly house                             |
| `build_date`         | `YYYY-MM-DD`   | `2026-02-11`   | Date of assembly                           |
| `batch_number`       | decimal        | `2`            | Production batch/lot number                |
| `siop_datecode`      | `YY/WW`        | `00/15`        | SCSI chip date code (BCD packed)           |
| `cpld_version`       | `major.minor`  | `1.3`          | CPLD bitstream version                     |
| `initial_fw_version` | `major.minor`  | `42.36`        | Firmware version flashed at factory        |
| `test_date`          | `YYYY-MM-DD`   | `2026-02-22`   | Date of factory testing                    |
| `test_fw_version`    | `major.minor`  | `42.36`        | Firmware version used for testing          |
| `test_status`        | hex bitmask    | `0x3F`         | Factory test results (see below)           |
| `owner_name`         | string (max 31)| (empty)        | Optional owner personalization             |

### Test Status Bits

| Bit | Mask   | Name    | Description                |
|-----|--------|---------|----------------------------|
| 0   | `0x01` | REGS    | Register read/write test   |
| 1   | `0x02` | IRQ     | Interrupt test             |
| 2   | `0x04` | SCSI    | SCSI bus test              |
| 3   | `0x08` | DMA     | DMA test                   |
| 4   | `0x10` | FLASH   | SPI flash test             |
| 5   | `0x20` | CPLD    | CPLD functionality test    |

A value of `0x3F` means all tests passed.

### PCB Color

The `pcb_color` field sets the bootmenu main screen color. It can be specified
as either a `#RGB` hex triplet (each digit 0-F) or a named alias:

| Alias    | Hex    | RGB444       | Description                       |
|----------|--------|--------------|-----------------------------------|
| `black`  | `#444` | (4, 4, 4)   | Subtle dark theme                 |
| `green`  | `#1A3` | (1, 10, 3)  | Classic PCB solder mask green     |
| `purple` | `#62C` | (6, 2, 12)  | OSHPark's signature purple        |
| `red`    | `#D22` | (13, 2, 2)  | Red solder mask                   |
| `white`  | `#DDD` | (13, 13, 13)| Almost white                      |
| `blue`   | `#68B` | (6, 8, 11)  | Default Amiga blue                |

Each component is a 4-bit value (0-15). The color is stored in RGB444 format
and also shown as RGB888 in `dumpmfg` output (multiply each component by 17).

Setting `pcb_color = #000` uses the default Amiga blue bootmenu color.

Color changes only affect the **main bootmenu screen**. Other pages (Disks,
Debug, About) remain Amiga blue and the DIP Switches page retains its red color.

### Example Config File

```
# A4092 Manufacturing Data
card_type = A4092
# comment out serial number to use S/serial.txt
# serial = 00000001
# Rev A = 1.0
# Rev C = 3.0
hw_revision = 3.0
pcb_color = #080
assembler_name = Stefan Reinauer
assembly_factory = JLCPCB
build_date = 2026-02-11
batch_number = 2
siop_datecode = 00/15
cpld_version = 1.3
initial_fw_version = 42.36
test_date = 2026-02-22
test_fw_version = 42.36
test_status = 0x3F
owner_name =
```

## Serial Number Handling

If the config file contains a `serial` key, that value is used directly. The
full serial string stored in flash is composed as `<card_type>-<serial>`,
e.g. `A4092-00000001`.

If the `serial` key is commented out or absent, `mfgtool` reads the next
serial number from `S/serial.txt`, uses it, and writes back the incremented
value. This allows batch programming of boards with sequential serial numbers.

The disk image ships with `S/serial.txt` initialized to `00000001`.

## Commands

### Manufacturing Commands

```
mfgtool writemfg <config_file>
```

Programs manufacturing data into flash:
1. Parses the config file
2. Assigns a serial number (from the file or auto-incremented from `S/serial.txt`)
3. Computes the integrity checksum
4. Erases the 4 KB manufacturing sector
5. Writes 256 bytes to flash
6. Reads back and verifies

```
mfgtool readmfg <config_file>
```

Reads manufacturing data from flash and writes it to a config file. Reports
a checksum warning if the data is corrupt.

```
mfgtool dumpmfg
```

Reads manufacturing data from flash and prints a human-readable dump to the
console, including decoded test flags, RGB888 color values, and checksum
status.

### SPI Flash Commands

```
mfgtool id                              # Show JEDEC flash ID
mfgtool read <addr> <len> <outfile>     # Read flash to file
mfgtool erase <addr> <len> [--verify]   # Erase 64K blocks
mfgtool write <addr> <infile> [--verify] # Program flash (assumes erased)
mfgtool verify <addr> <infile>          # Compare flash against file
mfgtool patch <addr> <hexbytes>         # Write hex bytes (assumes erased)
mfgtool status                          # Show SPI status registers
```

Addresses and lengths accept decimal or `0x` hex notation. Lengths also accept
`K` and `M` suffixes (e.g. `64K`, `1M`).

## Building

```
make mfgtool
```

Requires the `m68k-amigaos-gcc` cross-compiler toolchain.

## Disk Layout

On the distribution ADF disk image:

| Path             | Contents                        |
|------------------|----------------------------------|
| `Tools/mfgtool`  | The mfgtool binary               |
| `S/mfg.txt`      | Sample manufacturing config file |
| `S/serial.txt`   | Auto-increment serial counter    |

A typical factory programming session from the Amiga shell:

```
cd S:
Tools/mfgtool writemfg mfg.txt
Tools/mfgtool dumpmfg
```
