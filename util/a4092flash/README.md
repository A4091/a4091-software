# a4092flash

A flash programming utility for the A4092 SCSI controller.

## Features

- Read/write A4092 flash ROM
- Erase flash memory
- Probe flash contents
- NVRAM configuration (settings, DIP switches, bootmenu color)

## Usage

```
a4092flash [-Y] { -R <file> | -W <file> | -E | -P | -F <nvram_cmd> }
```

### Options

| Option | Description |
|--------|-------------|
| `-Y` | Assume YES to all prompts |
| `-R <file>` | Read flash contents to file |
| `-W <file>` | Write file to flash |
| `-E` | Erase entire flash |
| `-P` | Probe flash (show info) |
| `-B` | Reboot after operation |
| `-F <cmd>` | NVRAM operations |

## NVRAM Operations

The `-F` option supports semicolon-separated commands for managing NVRAM settings.

### Commands

| Command | Description |
|---------|-------------|
| `init` | Initialize NVRAM partition |
| `osflags` | Read OS flags |
| `switchflags` | Read switch flags |
| `color` | Read bootmenu color |
| `osflags=<hex>` | Write OS flags (hex value) |
| `switchflags=<hex>` | Write switch flags (hex value) |
| `color=<R>,<G>,<B>` | Set bootmenu color (0-15 each) |

### Examples

```bash
# Initialize NVRAM partition
a4092flash -F init

# Read all settings
a4092flash -F osflags;switchflags;color

# Write OS flags
a4092flash -F osflags=0f

# Initialize and write settings in one command
a4092flash -F init;osflags=0f;switchflags=00
```

## Bootmenu Color Customization

The A4092 bootmenu main screen color can be customized via NVRAM. Colors are
specified as RGB4 values (0-15 for each component).

### Reading Current Color

```bash
a4092flash -F color
```

### Setting Colors

```bash
a4092flash -F color=<R>,<G>,<B>
```

### Color Examples

| Color | Command | Description |
|-------|---------|-------------|
| Amiga Blue | `a4092flash -F color=6,8,11` | Default Amiga blue |
| PCB Black | `a4092flash -F color=3,3,3` | Subtle dark theme |
| PCB Green | `a4092flash -F color=1,10,3` | Classic PCB solder mask green |
| OSHPark Purple | `a4092flash -F color=6,2,12` | OSHPark's signature purple |
| PCB Red | `a4092flash -F color=13,2,2` | Red solder mask |
| PCB White | `a4092flash -F color=13,13,13` | Almost white |

### Reset to Default

Setting color to `0,0,0` restores the default Amiga blue:

```bash
a4092flash -F color=0,0,0
```

### Notes

- Color changes only affect the **main bootmenu screen**
- Other pages (Disks, Debug, About) remain Amiga blue
- The DIP Switches page retains its red color
- Changes take effect on next boot

## Flash Operations

### Update Firmware

```bash
a4092flash -W a4092.rom
```

### Backup Current Firmware

```bash
a4092flash -R backup.rom
```

### Check Installed Version

```bash
a4092flash -P
```

### Erase and Reflash

```bash
a4092flash -E -W a4092.rom
```

## Building

The utility is built as part of the main A4091/A4092 driver build:

```bash
make DEVICE=A4092
```

The resulting binary is `a4092flash` in the build output.
