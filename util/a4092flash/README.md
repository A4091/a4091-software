# a4092flash

A flash programming utility for the A4092 SCSI controller.

## Features

- Read/write A4092 flash ROM
- Erase flash memory
- Probe flash contents
- NVRAM configuration (settings, DIP switches)

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
| `osflags=<hex>` | Write OS flags (hex value) |
| `switchflags=<hex>` | Write switch flags (hex value) |

### Examples

```bash
# Initialize NVRAM partition
a4092flash -F init

# Read all settings
a4092flash -F osflags;switchflags

# Write OS flags
a4092flash -F osflags=0f

# Initialize and write settings in one command
a4092flash -F init;osflags=0f;switchflags=00
```

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
