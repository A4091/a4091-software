/*
 * A4091 utilities disk welcome banner.
 *
 * This replaces a long Startup-Sequence full of Echo commands with one
 * small process that writes the same text directly to the console.
 */

#include <dos/dos.h>
#include <exec/types.h>
#include <proto/dos.h>

#ifndef A4091_DISK_FULL_VERSION
#define A4091_DISK_FULL_VERSION "unknown"
#endif

#ifndef A4091_DISK_COPYRIGHT_YEAR
#define A4091_DISK_COPYRIGHT_YEAR "2026"
#endif

#define ARRAY_SIZE(x) (sizeof (x) / sizeof ((x)[0]))

typedef struct {
    const char *device_path;
    const char * const *banner;
} banner_def_t;

static BPTR out;

static const char * const banner_a4091[] = {
    "            ________  ___   ___  ________  ________    _____",
    "           |\\   __  \\|\\  \\ |\\  \\|\\   __  \\|\\  ___  \\  / __  \\",
    "           \\ \\  \\|\\  \\ \\  \\\\_\\  \\ \\  \\|\\  \\ \\____   \\|\\/_|\\  \\",
    "            \\ \\   __  \\ \\______  \\ \\  \\\\\\  \\|____|\\  \\|/ \\ \\  \\",
    "             \\ \\  \\ \\  \\|_____|\\  \\ \\  \\\\\\  \\  __\\_\\  \\   \\ \\  \\",
    "              \\ \\__\\ \\__\\     \\ \\__\\ \\_______\\|\\_______\\   \\ \\__\\",
    "               \\|__|\\|__|      \\|__|\\|_______|\\|_______|    \\|__|",
};

static const char * const banner_a4092[] = {
    "            ________  ___   ___  ________  ________    _______",
    "           |\\   __  \\|\\  \\ |\\  \\|\\   __  \\|\\  ___  \\  /  ___  \\",
    "           \\ \\  \\|\\  \\ \\  \\\\_\\  \\ \\  \\|\\  \\ \\____   \\/__/|_/  /|",
    "            \\ \\   __  \\ \\______  \\ \\  \\\\\\  \\|____|\\  \\__|//  / /",
    "             \\ \\  \\ \\  \\|_____|\\  \\ \\  \\\\\\  \\  __\\_\\  \\  /  /_/__",
    "              \\ \\__\\ \\__\\     \\ \\__\\ \\_______\\|\\_______\\|\\________\\",
    "               \\|__|\\|__|      \\|__|\\|_______|\\|_______| \\|_______|",
};

static const char * const banner_a4770[] = {
    "            ________  ___   ___  ________ ________  ________",
    "           |\\   __  \\|\\  \\ |\\  \\|\\_____  \\\\_____  \\|\\   __  \\",
    "           \\ \\  \\|\\  \\ \\  \\\\_\\  \\\\|___/  /\\|___/  /\\ \\  \\|\\  \\",
    "            \\ \\   __  \\ \\______  \\   /  / /   /  / /\\ \\  \\\\\\  \\",
    "             \\ \\  \\ \\  \\|_____|\\  \\ /  / /   /  / /  \\ \\  \\\\\\  \\",
    "              \\ \\__\\ \\__\\     \\ \\__Y__/ /   /__/ /    \\ \\_______\\",
    "               \\|__|\\|__|      \\|__|__|/    |__|/      \\|_______|",
};

static const char * const banner_scsi710[] = {
    "          ________  ___   ___  ________  ________  ________  _________",
    "         |\\   __  \\|\\  \\ |\\  \\|\\   __  \\|\\   __  \\|\\   __  \\|\\___   ___\\",
    "         \\ \\  \\|\\  \\ \\  \\\\_\\  \\ \\  \\|\\  \\ \\  \\|\\  \\ \\  \\|\\  \\|___ \\  \\_|",
    "          \\ \\   __  \\ \\______  \\ \\  \\\\\\  \\ \\  \\\\\\  \\ \\  \\\\\\  \\   \\ \\  \\",
    "           \\ \\  \\ \\  \\|_____|\\  \\ \\  \\\\\\  \\ \\  \\\\\\  \\ \\  \\\\\\  \\   \\ \\  \\",
    "            \\ \\__\\ \\__\\     \\ \\__\\ \\_______\\ \\_______\\ \\_______\\   \\ \\__\\",
    "             \\|__|\\|__|      \\|__|\\|_______|\\|_______|\\|_______|    \\|__|",
};

static const char * const banner_scsi770[] = {
    "            ________  ________  ________ _________  ___   ___  ___  __",
    "           |\\   ____\\|\\   __  \\|\\  _____\\\\___   ___\\\\  \\ |\\  \\|\\  \\|\\  \\",
    "           \\ \\  \\___|\\ \\  \\|\\  \\ \\  \\__/\\|___ \\  \\_\\ \\  \\\\_\\  \\ \\  \\/  /|_",
    "            \\ \\_____  \\ \\   __  \\ \\   __\\    \\ \\  \\ \\ \\______  \\ \\   ___  \\",
    "             \\|____|\\  \\ \\  \\ \\  \\ \\  \\_|     \\ \\  \\ \\|_____|\\  \\ \\  \\\\ \\  \\",
    "               ____\\_\\  \\ \\__\\ \\__\\ \\__\\       \\ \\__\\       \\ \\__\\ \\__\\\\ \\__\\",
    "              |\\_________\\|__|\\|__|\\|__|        \\|__|        \\|__|\\|__| \\|__|",
    "              \\|_________|",
};

static const char * const banner_unknown[] = {
    "            ________  ________  ________  ___      _____ ______   _______",
    "           |\\   ____\\|\\   ____\\|\\   ____\\|\\  \\    |\\   _ \\  _   \\|\\  ___ \\",
    "           \\ \\  \\___|\\ \\  \\___|\\ \\  \\___|\\ \\  \\   \\ \\  \\\\\\__\\ \\  \\ \\   __/|",
    "            \\ \\_____  \\ \\  \\    \\ \\_____  \\ \\  \\   \\ \\  \\\\|__| \\  \\ \\  \\_|/__",
    "             \\|____|\\  \\ \\  \\____\\|____|\\  \\ \\  \\ __\\ \\  \\    \\ \\  \\ \\  \\_|\\ \\",
    "               ____\\_\\  \\ \\_______\\____\\_\\  \\ \\__\\\\__\\ \\__\\    \\ \\__\\ \\_______\\",
    "              |\\_________\\|_______|\\_________\\|__\\|__|\\|__|     \\|__|\\|_______|",
    "              \\|_________|        \\|_________|",
};

static const banner_def_t banners[] = {
    { "devs:a4091.device",   banner_a4091 },
    { "devs:a4092.device",   banner_a4092 },
    { "devs:a4770.device",   banner_a4770 },
    { "devs:scsi710.device", banner_scsi710 },
    { "devs:scsi770.device", banner_scsi770 },
};

static const char * const common_lines[] = {
    "                 Welcome to the SCSI Test and Utilities Disk.",
    "",
    "     This disk contains the following tools:",
    "",
    "       ncr7xx     - NCR53C7x0 SCSI controller test utility",
    "       devtest    - Storage device benchmark tool",
    "       rdb        - Modify your Rigid Disk Block settings",
    "       RDBFlags   - Easily toggle LastDisk flag in RDB",
};

static const char * const flash_a4092_lines[] = {
    "       a4092flash - Firmware Update Utility for A4092",
    "       update     - Flash the bundled ROM image onto the controller",
};

static const char * const flash_a4770_lines[] = {
    "       a4092flash - Firmware Update Utility for A4770",
    "       update     - Flash the bundled ROM image onto the controller",
};

static const char * const final_lines[] = {
    "       scsifix    - Redirects scsi.device access for convenience.",
    "",
};

static unsigned int
str_len(const char *str)
{
    const char *ptr = str;

    while (*ptr != '\0')
        ptr++;
    return ((unsigned int)(ptr - str));
}

static void
write_text(const char *text)
{
    Write(out, (APTR)text, str_len(text));
}

static void
write_newline(void)
{
    static const char newline[] = "\n";

    Write(out, (APTR)newline, 1);
}

static void
write_line(const char *line)
{
    write_text(line);
    write_newline();
}

static void
write_spaces(unsigned int count)
{
    static const char spaces[] =
        "                                                                                ";

    while (count > 0) {
        unsigned int chunk = count;

        if (chunk > sizeof (spaces) - 1)
            chunk = sizeof (spaces) - 1;
        Write(out, (APTR)spaces, chunk);
        count -= chunk;
    }
}

static void
write_centered_line(const char *line)
{
    unsigned int len = str_len(line);

    if (len < 80)
        write_spaces((80 - len) / 2);
    write_line(line);
}

static void
write_lines(const char * const *lines, unsigned int count)
{
    unsigned int pos;

    for (pos = 0; pos < count; pos++)
        write_line(lines[pos]);
}

static int
path_exists(const char *path)
{
    BPTR lock = Lock((STRPTR)path, ACCESS_READ);

    if (lock == 0)
        return (0);
    UnLock(lock);
    return (1);
}

static const char * const *
select_banner(void)
{
    unsigned int pos;

    for (pos = 0; pos < ARRAY_SIZE(banners); pos++) {
        if (path_exists(banners[pos].device_path))
            return (banners[pos].banner);
    }
    return (banner_unknown);
}

int
main(void)
{
    static const char clear_screen[] = "\x1b[0;0H\x1b[2J\n";
    static const char version[] = "v" A4091_DISK_FULL_VERSION;
    static const char copyright[] =
        "\xa9 2023-" A4091_DISK_COPYRIGHT_YEAR
        " Stefan Reinauer & Chris Hooper";
    const char * const *banner;

    out = Output();
    if (out == 0)
        return (20);

    Write(out, (APTR)clear_screen, sizeof (clear_screen) - 1);

    banner = select_banner();
    if (banner == banner_scsi770 || banner == banner_unknown)
        write_lines(banner, 8);
    else
        write_lines(banner, 7);

    write_newline();
    write_centered_line(version);
    write_centered_line(copyright);
    write_newline();

    write_lines(common_lines, ARRAY_SIZE(common_lines));
    if (path_exists("devs:a4092.device"))
        write_lines(flash_a4092_lines, ARRAY_SIZE(flash_a4092_lines));
    else if (path_exists("devs:a4770.device"))
        write_lines(flash_a4770_lines, ARRAY_SIZE(flash_a4770_lines));
    write_lines(final_lines, ARRAY_SIZE(final_lines));

    return (0);
}
