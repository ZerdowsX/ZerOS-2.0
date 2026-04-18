#include <stdint.h>
#include <stddef.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_ADDRESS 0xB8000
#define CONSOLE_TOP 1

#define PORT_KEYBOARD_DATA 0x60
#define PORT_KEYBOARD_STATUS 0x64
#define PORT_CMOS_INDEX 0x70
#define PORT_CMOS_DATA 0x71
#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376
#define INSTALL_LBA 2048
#define FS_LBA 2049

#define MAX_INPUT 128
#define MAX_FILES 16
#define FILE_DATA_SIZE 96
#define FS_PERSIST_MAX_FILES 8
#define FS_PERSIST_NAME_SIZE 24
#define FS_PERSIST_DATA_SIZE 28

static volatile uint16_t* const vga = (uint16_t*)VGA_ADDRESS;
static size_t cursor_row = 0;
static size_t cursor_col = 0;

// default: white text on green background
static uint8_t current_color = 0x2F;

static uint32_t boot_mem_lower_kb = 0;
static uint32_t boot_mem_upper_kb = 0;

struct file_entry {
    char name[24];
    char data[FILE_DATA_SIZE];
    uint8_t used;
    uint8_t important;
};

static struct file_entry fs[MAX_FILES];
static int fs_save_to_disk(const char* disk_name);
static int fs_load_from_disk(const char* disk_name);
static int disk_has_install_marker(const char* disk_name);
static void show_gsod(const char* reason);
static void enter_recovery_mode(const char* reason);
static void draw_status_bar(void);
static void cmd_time(void);

static int persistent_mode = 0;
static char active_disk_name[8] = "disk0";
static int recovery_mode = 0;

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
};

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void io_wait(void) {
    outb(0x80, 0);
}

static size_t str_len(const char* s) {
    size_t len = 0;
    while (s[len]) ++len;
    return len;
}

static int streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        ++str;
        ++prefix;
    }
    return 1;
}

static void str_copy(char* dst, const char* src, size_t max) {
    if (max == 0) return;
    size_t i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static int str_n_equal(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static const char* skip_spaces(const char* s) {
    while (*s == ' ') ++s;
    return s;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

static void scroll_if_needed(void) {
    if (cursor_row < VGA_HEIGHT) return;

    for (size_t y = CONSOLE_TOP + 1; y < VGA_HEIGHT; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            vga[(y - 1) * VGA_WIDTH + x] = vga[y * VGA_WIDTH + x];
        }
    }

    for (size_t x = 0; x < VGA_WIDTH; ++x) {
        vga[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = ((uint16_t)current_color << 8) | ' ';
    }

    cursor_row = VGA_HEIGHT - 1;
}

static void put_char(char c) {
    if (c == '\n') {
        cursor_col = 0;
        ++cursor_row;
        scroll_if_needed();
        return;
    }

    vga[cursor_row * VGA_WIDTH + cursor_col] = ((uint16_t)current_color << 8) | (uint8_t)c;

    ++cursor_col;
    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        ++cursor_row;
        scroll_if_needed();
    }
}

static void print(const char* s) {
    while (*s) put_char(*s++);
}

static void print_dec(uint32_t value) {
    char buffer[16];
    size_t i = 0;

    if (value == 0) {
        put_char('0');
        return;
    }

    while (value > 0 && i < sizeof(buffer)) {
        buffer[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0) put_char(buffer[--i]);
}

static void print_2d(uint8_t value) {
    put_char((char)('0' + (value / 10)));
    put_char((char)('0' + (value % 10)));
}

static void clear_screen(void) {
    for (size_t y = 0; y < VGA_HEIGHT; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            vga[y * VGA_WIDTH + x] = ((uint16_t)current_color << 8) | ' ';
        }
    }
    cursor_row = CONSOLE_TOP;
    cursor_col = 0;
}

static char scancode_to_ascii(uint8_t sc) {
    static const char map[128] = {
        [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
        [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
        [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
        [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
        [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
        [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
        [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
        [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
        [0x28] = '\'', [0x29] = '`',
        [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
        [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.',
        [0x35] = '/', [0x39] = ' '
    };

    if (sc >= 128) return 0;
    return map[sc];
}

static char get_keypress(void) {
    while (1) {
        while ((inb(PORT_KEYBOARD_STATUS) & 1) == 0) { }

        uint8_t scancode = inb(PORT_KEYBOARD_DATA);
        if (scancode & 0x80) continue;

        char c = scancode_to_ascii(scancode);
        if (c) return c;
    }
}

static void read_line(char* out, size_t max) {
    size_t idx = 0;
    while (1) {
        draw_status_bar();
        char c = get_keypress();
        if (c == '\n') {
            put_char('\n');
            out[idx] = '\0';
            return;
        }
        if (c == '\b') {
            if (idx > 0 && cursor_col > 0) {
                --idx;
                --cursor_col;
                vga[cursor_row * VGA_WIDTH + cursor_col] = ((uint16_t)current_color << 8) | ' ';
            }
            continue;
        }
        if (idx < max - 1) {
            out[idx++] = c;
            put_char(c);
        }
    }
}

static void fs_add_file(const char* name, const char* data, uint8_t important) {
    for (size_t i = 0; i < MAX_FILES; ++i) {
        if (!fs[i].used) {
            fs[i].used = 1;
            fs[i].important = important;
            str_copy(fs[i].name, name, sizeof(fs[i].name));
            str_copy(fs[i].data, data, sizeof(fs[i].data));
            return;
        }
    }
}

static void fs_init(void) {
    for (size_t i = 0; i < MAX_FILES; ++i) fs[i].used = 0;

    fs_add_file("system.cfg", "theme=green", 1);
    fs_add_file("boot.log", "ZerDOS boot success", 1);
    fs_add_file("readme.txt", "Type help for commands", 0);
}

static struct file_entry* fs_find(const char* name) {
    for (size_t i = 0; i < MAX_FILES; ++i) {
        if (fs[i].used && streq(fs[i].name, name)) return &fs[i];
    }
    return 0;
}

static int fs_remove(const char* name) {
    struct file_entry* f = fs_find(name);
    if (!f) return 0;
    f->used = 0;
    f->important = 0;
    f->name[0] = '\0';
    f->data[0] = '\0';
    return 1;
}

static int fs_write_text(const char* name, const char* data) {
    struct file_entry* f = fs_find(name);
    if (!f) return 0;
    str_copy(f->data, data, sizeof(f->data));
    return 1;
}

static int fs_has_required_system_files(void) {
    return fs_find("system.cfg") != 0 && fs_find("boot.log") != 0;
}

static void fs_sync_if_persistent(void) {
    if (!persistent_mode) return;
    if (!fs_save_to_disk(active_disk_name)) {
        enter_recovery_mode("Persistent FS sync failed");
    }
}

static void cmd_fs(const char* args) {
    if (streq(args, "") || streq(args, "ls")) {
        print("FS files:\n");
        for (size_t i = 0; i < MAX_FILES; ++i) {
            if (fs[i].used) {
                print(" - ");
                print(fs[i].name);
                if (fs[i].important) print(" [important]");
                print("\n");
            }
        }
        print("Usage: fs ls | fs cat <name> | fs touch <name> | fs write <name> <text>\n");
        print("       fs rm <name> | fs save | fs load\n");
        return;
    }

    if (starts_with(args, "cat ")) {
        const char* name = args + 4;
        struct file_entry* f = fs_find(name);
        if (!f) {
            print("File not found\n");
            return;
        }
        print("[");
        print(f->name);
        print("] ");
        print(f->data);
        print("\n");
        return;
    }

    if (starts_with(args, "touch ")) {
        const char* name = args + 6;
        if (str_len(name) == 0) {
            print("Name required\n");
            return;
        }
        if (fs_find(name)) {
            print("File already exists\n");
            return;
        }
        fs_add_file(name, "", 0);
        fs_sync_if_persistent();
        print("File created\n");
        return;
    }

    if (starts_with(args, "write ")) {
        const char* p = args + 6;
        p = skip_spaces(p);
        if (*p == '\0') {
            print("Usage: fs write <name> <text>\n");
            return;
        }

        char name[24];
        size_t ni = 0;
        while (*p && *p != ' ' && ni < sizeof(name) - 1) {
            name[ni++] = *p++;
        }
        name[ni] = '\0';
        p = skip_spaces(p);
        if (*p == '\0') {
            print("Text required\n");
            return;
        }
        if (!fs_write_text(name, p)) {
            print("File not found\n");
            return;
        }
        fs_sync_if_persistent();
        print("File updated\n");
        return;
    }

    if (starts_with(args, "rm ")) {
        const char* name = args + 3;
        if (!fs_remove(name)) {
            print("File not found\n");
            return;
        }
        fs_sync_if_persistent();
        if (!fs_has_required_system_files()) {
            enter_recovery_mode("Critical system file deleted");
        }
        print("File removed\n");
        return;
    }

    if (streq(args, "save")) {
        if (fs_save_to_disk(active_disk_name)) print("FS saved\n");
        else print("FS save failed (disk unavailable?)\n");
        if (fs_save_to_disk("disk0")) print("FS saved to disk0\n");
        else print("FS save failed (disk0 unavailable?)\n");
        return;
    }

    if (streq(args, "load")) {
        if (fs_load_from_disk(active_disk_name)) print("FS loaded\n");
        if (fs_load_from_disk("disk0")) print("FS loaded from disk0\n");
        else print("FS load failed or not initialized\n");
        return;
    }

    print("Unknown fs command\n");
}

static void cmd_mem(void) {
    print("Memory report:\n");
    print(" lower KB: ");
    print_dec(boot_mem_lower_kb);
    print("\n upper KB: ");
    print_dec(boot_mem_upper_kb);
    print("\n approx MB: ");
    print_dec((boot_mem_lower_kb + boot_mem_upper_kb) / 1024);
    print("\n");
}

static int parse_u8(const char* s, uint8_t* out) {
    if (!s || *s == '\0') return 0;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > 255) return 0;
        ++s;
    }
    if (*s != '\0') return 0;
    *out = (uint8_t)v;
    return 1;
}

static void cmd_color(const char* args) {
    if (streq(args, "") || streq(args, "help")) {
        print("Usage: color <fg 0-15> <bg 0-15>\n");
        return;
    }
    char a[8], b[8];
    size_t i = 0, j = 0;
    while (*args == ' ') ++args;
    while (*args && *args != ' ' && i < sizeof(a) - 1) a[i++] = *args++;
    a[i] = '\0';
    while (*args == ' ') ++args;
    while (*args && *args != ' ' && j < sizeof(b) - 1) b[j++] = *args++;
    b[j] = '\0';
    uint8_t fg, bg;
    if (!parse_u8(a, &fg) || !parse_u8(b, &bg) || fg > 15 || bg > 15) {
        print("Invalid colors. Use numbers 0..15\n");
        return;
    }
    current_color = (uint8_t)((bg << 4) | fg);
    clear_screen();
    draw_status_bar();
    draw_status_bar();
    print("Color changed.\n");
}

static void cmd_pcinfo(void) {
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0));
    ((uint32_t*)vendor)[0] = ebx;
    ((uint32_t*)vendor)[1] = edx;
    ((uint32_t*)vendor)[2] = ecx;
    vendor[12] = '\0';

    print("PC info:\n");
    print(" CPU vendor: ");
    print(vendor);
    print("\n RAM upper KB: ");
    print_dec(boot_mem_upper_kb);
    print("\n");
}

static void cmd_zeron(const char* args) {
    if (streq(args, "") || streq(args, "help")) {
        print("zeron help:\n");
        print("  zeron edit <file>\n");
        print("  zeron run <file>\n");
        print("  zeron compile <src> <dst>\n");
        print("Tags in scripts:\n");
        print("  [PRINT:text] [ECHO:text] [CLEAR] [TIME] [PCINFO]\n");
        print("  [COLOR:fg,bg] [TOUCH:name] [CAT:name] [WRITE:name,text]\n");
        return;
    }

    if (starts_with(args, "edit ")) {
        const char* name = args + 5;
        char script[FILE_DATA_SIZE];
        print("Zeron editor: one-line script, then Enter.\n");
        print("Example: [PRINT:Hello];[CLEAR];[PRINT:Done]\n");
        print("script> ");
        read_line(script, sizeof(script));
        if (fs_find(name)) fs_write_text(name, script);
        else fs_add_file(name, script, 0);
        fs_sync_if_persistent();
        print("Saved script file.\n");
        return;
    }

    if (starts_with(args, "run ")) {
        const char* name = args + 4;
        struct file_entry* f = fs_find(name);
        if (!f) {
            print("Script file not found\n");
            return;
        }
        const char* s = f->data;
        while (*s) {
            while (*s == ' ' || *s == ';') ++s;
            if (starts_with(s, "[PRINT:")) {
                s += 7;
                while (*s && *s != ']') put_char(*s++);
                print("\n");
            } else if (starts_with(s, "[ECHO:")) {
                s += 6;
                while (*s && *s != ']') put_char(*s++);
            } else if (starts_with(s, "[TIME]")) {
                cmd_time();
                s += 6;
            } else if (starts_with(s, "[PCINFO]")) {
                cmd_pcinfo();
                s += 8;
            } else if (starts_with(s, "[COLOR:")) {
                s += 7;
                char fg_s[4], bg_s[4];
                size_t fi = 0, bi = 0;
                while (*s == ' ') ++s;
                while (*s && *s != ',' && *s != ']' && fi < sizeof(fg_s) - 1) fg_s[fi++] = *s++;
                fg_s[fi] = '\0';
                if (*s == ',') ++s;
                while (*s == ' ') ++s;
                while (*s && *s != ']' && bi < sizeof(bg_s) - 1) bg_s[bi++] = *s++;
                bg_s[bi] = '\0';
                uint8_t fg, bg;
                if (parse_u8(fg_s, &fg) && parse_u8(bg_s, &bg) && fg <= 15 && bg <= 15) {
                    current_color = (uint8_t)((bg << 4) | fg);
                }
            } else if (starts_with(s, "[TOUCH:")) {
                s += 7;
                char fname[24];
                size_t fi = 0;
                while (*s && *s != ']' && fi < sizeof(fname) - 1) fname[fi++] = *s++;
                fname[fi] = '\0';
                if (fname[0] && !fs_find(fname)) fs_add_file(fname, "", 0);
                fs_sync_if_persistent();
            } else if (starts_with(s, "[CAT:")) {
                s += 5;
                char fname[24];
                size_t fi = 0;
                while (*s && *s != ']' && fi < sizeof(fname) - 1) fname[fi++] = *s++;
                fname[fi] = '\0';
                struct file_entry* cf = fs_find(fname);
                if (cf) {
                    print("[");
                    print(cf->name);
                    print("] ");
                    print(cf->data);
                    print("\n");
                }
            } else if (starts_with(s, "[WRITE:")) {
                s += 7;
                char fname[24], text[FILE_DATA_SIZE];
                size_t fi = 0, ti = 0;
                while (*s && *s != ',' && *s != ']' && fi < sizeof(fname) - 1) fname[fi++] = *s++;
                fname[fi] = '\0';
                if (*s == ',') ++s;
                while (*s && *s != ']' && ti < sizeof(text) - 1) text[ti++] = *s++;
                text[ti] = '\0';
                if (fname[0]) {
                    if (fs_find(fname)) fs_write_text(fname, text);
                    else fs_add_file(fname, text, 0);
                    fs_sync_if_persistent();
                }
            } else if (starts_with(s, "[CLEAR]")) {
                clear_screen();
                draw_status_bar();
                s += 7;
            } else {
                while (*s && *s != ';') ++s;
            }
            while (*s && *s != ';') ++s;
            if (*s == ';') ++s;
        }
        return;
    }

    if (starts_with(args, "compile ")) {
        const char* p = args + 8;
        char src[24], dst[24];
        size_t i = 0, j = 0;
        while (*p == ' ') ++p;
        while (*p && *p != ' ' && i < sizeof(src) - 1) src[i++] = *p++;
        src[i] = '\0';
        while (*p == ' ') ++p;
        while (*p && *p != ' ' && j < sizeof(dst) - 1) dst[j++] = *p++;
        dst[j] = '\0';
        struct file_entry* f = fs_find(src);
        if (!f || dst[0] == '\0') {
            print("Usage: zeron compile <src> <dst>\n");
            return;
        }
        char out[FILE_DATA_SIZE];
        str_copy(out, "BIN:", sizeof(out));
        size_t base = str_len(out);
        str_copy(out + base, f->data, sizeof(out) - base);
        if (fs_find(dst)) fs_write_text(dst, out);
        else fs_add_file(dst, out, 0);
        fs_sync_if_persistent();
        print("Compiled and saved.\n");
        return;
    }

    print("zeron commands: zeron edit <file> | zeron run <file> | zeron compile <src> <dst>\n");
}

static void cmd_time(void) {
    outb(PORT_CMOS_INDEX, 0x00);
    uint8_t sec = inb(PORT_CMOS_DATA);
    outb(PORT_CMOS_INDEX, 0x02);
    uint8_t min = inb(PORT_CMOS_DATA);
    outb(PORT_CMOS_INDEX, 0x04);
    uint8_t hour = inb(PORT_CMOS_DATA);

    sec = bcd_to_bin(sec);
    min = bcd_to_bin(min);
    hour = bcd_to_bin(hour);

    print("RTC time: ");
    print_2d(hour);
    put_char(':');
    print_2d(min);
    put_char(':');
    print_2d(sec);
    print("\n");
}

static void read_rtc_datetime(uint8_t* hour, uint8_t* min, uint8_t* sec, uint8_t* day, uint8_t* mon, uint8_t* year) {
    outb(PORT_CMOS_INDEX, 0x00); *sec = bcd_to_bin(inb(PORT_CMOS_DATA));
    outb(PORT_CMOS_INDEX, 0x02); *min = bcd_to_bin(inb(PORT_CMOS_DATA));
    outb(PORT_CMOS_INDEX, 0x04); *hour = bcd_to_bin(inb(PORT_CMOS_DATA));
    outb(PORT_CMOS_INDEX, 0x07); *day = bcd_to_bin(inb(PORT_CMOS_DATA));
    outb(PORT_CMOS_INDEX, 0x08); *mon = bcd_to_bin(inb(PORT_CMOS_DATA));
    outb(PORT_CMOS_INDEX, 0x09); *year = bcd_to_bin(inb(PORT_CMOS_DATA));
}

static void draw_status_bar(void) {
    uint8_t h, m, s, d, mo, y;
    read_rtc_datetime(&h, &m, &s, &d, &mo, &y);

    char buf[80];
    size_t i = 0;
    const char* mode = recovery_mode ? "RECOVERY" : (persistent_mode ? "PERSIST" : "LIVE");
    const char* head = " ZerDOS ";
    while (*head && i < sizeof(buf) - 1) buf[i++] = *head++;
    buf[i++] = '|'; buf[i++] = ' ';
    const char* mtxt = mode;
    while (*mtxt && i < sizeof(buf) - 1) buf[i++] = *mtxt++;
    buf[i++] = ' '; buf[i++] = '|'; buf[i++] = ' ';
    buf[i++] = '2'; buf[i++] = '0'; buf[i++] = '0' + (y / 10); buf[i++] = '0' + (y % 10);
    buf[i++] = '-'; buf[i++] = '0' + (mo / 10); buf[i++] = '0' + (mo % 10);
    buf[i++] = '-'; buf[i++] = '0' + (d / 10); buf[i++] = '0' + (d % 10);
    buf[i++] = ' '; buf[i++] = '0' + (h / 10); buf[i++] = '0' + (h % 10);
    buf[i++] = ':'; buf[i++] = '0' + (m / 10); buf[i++] = '0' + (m % 10);
    buf[i++] = ':'; buf[i++] = '0' + (s / 10); buf[i++] = '0' + (s % 10);
    buf[i] = '\0';

    uint8_t bar_color = 0x1F; // white on blue
    for (size_t x = 0; x < VGA_WIDTH; ++x) {
        char c = (x < i) ? buf[x] : ' ';
        vga[x] = ((uint16_t)bar_color << 8) | (uint8_t)c;
    }
}

static int ata_wait_not_busy(uint16_t io) {
    for (uint32_t i = 0; i < 1000000; ++i) {
        if ((inb(io + 7) & 0x80) == 0) return 1;
    }
    return 0;
}

static int ata_wait_drq(uint16_t io) {
    for (uint32_t i = 0; i < 1000000; ++i) {
        uint8_t s = inb(io + 7);
        if (s & 0x01) return 0;      // ERR
        if (s & 0x08) return 1;      // DRQ
    }
    return 0;
}

static int ata_write_sector_pio(uint16_t io, uint16_t ctrl, uint8_t drive, uint32_t lba, const uint8_t* data) {
    (void)ctrl;
    if (!ata_wait_not_busy(io)) return 0;

    outb(io + 6, (uint8_t)(0xE0 | (drive << 4) | ((lba >> 24) & 0x0F)));
    io_wait();
    outb(io + 2, 1); // sectors count
    outb(io + 3, (uint8_t)(lba & 0xFF));
    outb(io + 4, (uint8_t)((lba >> 8) & 0xFF));
    outb(io + 5, (uint8_t)((lba >> 16) & 0xFF));
    outb(io + 7, 0x30); // WRITE SECTORS

    if (!ata_wait_drq(io)) return 0;

    for (size_t i = 0; i < 256; ++i) {
        uint16_t w = (uint16_t)data[i * 2] | ((uint16_t)data[i * 2 + 1] << 8);
        outw(io, w);
    }

    if (!ata_wait_not_busy(io)) return 0;
    if (inb(io + 7) & 0x01) return 0;
    return 1;
}

static int ata_read_sector_pio(uint16_t io, uint16_t ctrl, uint8_t drive, uint32_t lba, uint8_t* out) {
    (void)ctrl;
    if (!ata_wait_not_busy(io)) return 0;

    outb(io + 6, (uint8_t)(0xE0 | (drive << 4) | ((lba >> 24) & 0x0F)));
    io_wait();
    outb(io + 2, 1);
    outb(io + 3, (uint8_t)(lba & 0xFF));
    outb(io + 4, (uint8_t)((lba >> 8) & 0xFF));
    outb(io + 5, (uint8_t)((lba >> 16) & 0xFF));
    outb(io + 7, 0x20); // READ SECTORS

    if (!ata_wait_drq(io)) return 0;

    for (size_t i = 0; i < 256; ++i) {
        uint16_t w = inw(io);
        out[i * 2] = (uint8_t)(w & 0xFF);
        out[i * 2 + 1] = (uint8_t)(w >> 8);
    }

    if (inb(io + 7) & 0x01) return 0;
    return 1;
}

static int disk_select(const char* disk_name, uint16_t* io, uint16_t* ctrl, uint8_t* drive) {
    if (streq(disk_name, "disk0")) {
        *io = ATA_PRIMARY_IO; *ctrl = ATA_PRIMARY_CTRL; *drive = 0; return 1;
    }
    if (streq(disk_name, "disk1")) {
        *io = ATA_PRIMARY_IO; *ctrl = ATA_PRIMARY_CTRL; *drive = 1; return 1;
    }
    if (streq(disk_name, "disk2")) {
        *io = ATA_SECONDARY_IO; *ctrl = ATA_SECONDARY_CTRL; *drive = 0; return 1;
    }
    return 0;
}

static int fs_save_to_disk(const char* disk_name) {
    uint16_t io, ctrl;
    uint8_t drive;
    if (!disk_select(disk_name, &io, &ctrl, &drive)) return 0;

    uint8_t sector[512];
    for (size_t i = 0; i < sizeof(sector); ++i) sector[i] = 0;

    sector[0] = 'Z';
    sector[1] = 'F';
    sector[2] = 'S';
    sector[3] = '1';

    size_t src_i = 0;
    size_t saved = 0;
    size_t offset = 5;
    while (src_i < MAX_FILES && saved < FS_PERSIST_MAX_FILES) {
        if (fs[src_i].used) {
            size_t nlen = str_len(fs[src_i].name);
            size_t dlen = str_len(fs[src_i].data);
            if (nlen > FS_PERSIST_NAME_SIZE) nlen = FS_PERSIST_NAME_SIZE;
            if (dlen > FS_PERSIST_DATA_SIZE) dlen = FS_PERSIST_DATA_SIZE;

            sector[offset + 0] = 1;
            sector[offset + 1] = fs[src_i].important ? 1 : 0;
            sector[offset + 2] = (uint8_t)nlen;
            sector[offset + 3] = (uint8_t)dlen;
            for (size_t j = 0; j < nlen; ++j) sector[offset + 4 + j] = (uint8_t)fs[src_i].name[j];
            for (size_t j = 0; j < dlen; ++j) sector[offset + 4 + FS_PERSIST_NAME_SIZE + j] = (uint8_t)fs[src_i].data[j];

            offset += 4 + FS_PERSIST_NAME_SIZE + FS_PERSIST_DATA_SIZE;
            ++saved;
        }
        ++src_i;
    }
    sector[4] = (uint8_t)saved;

    return ata_write_sector_pio(io, ctrl, drive, FS_LBA, sector);
}

static int fs_load_from_disk(const char* disk_name) {
    uint16_t io, ctrl;
    uint8_t drive;
    if (!disk_select(disk_name, &io, &ctrl, &drive)) return 0;

    uint8_t sector[512];
    if (!ata_read_sector_pio(io, ctrl, drive, FS_LBA, sector)) return 0;
    if (!(sector[0] == 'Z' && sector[1] == 'F' && sector[2] == 'S' && sector[3] == '1')) return 0;

    for (size_t i = 0; i < MAX_FILES; ++i) fs[i].used = 0;

    size_t count = sector[4];
    if (count > FS_PERSIST_MAX_FILES) count = FS_PERSIST_MAX_FILES;
    size_t offset = 5;
    for (size_t i = 0; i < count && i < MAX_FILES; ++i) {
        uint8_t used = sector[offset + 0];
        uint8_t important = sector[offset + 1];
        size_t nlen = sector[offset + 2];
        size_t dlen = sector[offset + 3];
        if (nlen > FS_PERSIST_NAME_SIZE) nlen = FS_PERSIST_NAME_SIZE;
        if (dlen > FS_PERSIST_DATA_SIZE) dlen = FS_PERSIST_DATA_SIZE;

        if (used) {
            fs[i].used = 1;
            fs[i].important = important;
            for (size_t j = 0; j < nlen; ++j) fs[i].name[j] = (char)sector[offset + 4 + j];
            fs[i].name[nlen] = '\0';
            for (size_t j = 0; j < dlen; ++j) fs[i].data[j] = (char)sector[offset + 4 + FS_PERSIST_NAME_SIZE + j];
            fs[i].data[dlen] = '\0';
        }

        offset += 4 + FS_PERSIST_NAME_SIZE + FS_PERSIST_DATA_SIZE;
    }
    return 1;
}

static int disk_has_install_marker(const char* disk_name) {
    uint16_t io, ctrl;
    uint8_t drive;
    if (!disk_select(disk_name, &io, &ctrl, &drive)) return 0;

    uint8_t sector[512];
    if (!ata_read_sector_pio(io, ctrl, drive, INSTALL_LBA, sector)) return 0;
    return str_n_equal((const char*)sector, "ZERDOS_INSTALL_V1", 16);
}

static int do_real_install(const char* disk_name) {
    uint16_t io, ctrl;
    uint8_t drive;
    if (!disk_select(disk_name, &io, &ctrl, &drive)) return 0;

    uint8_t sector[512];
    uint8_t verify[512];
    for (size_t i = 0; i < 512; ++i) sector[i] = 0;

    const char* sig = "ZERDOS_INSTALL_V1";
    for (size_t i = 0; sig[i] && i < 32; ++i) sector[i] = (uint8_t)sig[i];
    for (size_t i = 0; disk_name[i] && i < 24; ++i) sector[64 + i] = (uint8_t)disk_name[i];

    // Write to LBA 2048, avoiding MBR/GPT headers.
    if (!ata_write_sector_pio(io, ctrl, drive, INSTALL_LBA, sector)) return 0;
    if (!ata_read_sector_pio(io, ctrl, drive, INSTALL_LBA, verify)) return 0;

    for (size_t i = 0; i < 16; ++i) {
        if (verify[i] != sector[i]) return 0;
    }
    return 1;
}

static void show_gsod(const char* reason) {
    current_color = 0x74; // red text on gray background
    clear_screen();
    print("*** ZerDOS GREY SCREEN OF DEATH ***\n\n");
    print("A critical error occurred:\n");
    print(reason);
    print("\n\nSystem halted. Reboot required.\n");
    while (1) {
        __asm__ volatile ("hlt");
    }
}

static void enter_recovery_mode(const char* reason) {
    recovery_mode = 1;
    clear_screen();
    draw_status_bar();
    print("=== RECOVERY MODE ===\n");
    print("Reason: ");
    print(reason);
    print("\nRestoring core files...\n");
    if (!fs_find("system.cfg")) fs_add_file("system.cfg", "theme=green", 1);
    if (!fs_find("boot.log")) fs_add_file("boot.log", "Recovered after fault", 1);
    fs_sync_if_persistent();
    print("Recovery finished. Type 'recovery' or continue working.\n");
}

static void cmd_help(void) {
    print("Commands:\n");
    print("  help                - show this list\n");
    print("  clear               - clear screen\n");
    print("  hello               - quick test\n");
    print("  time | vrem         - show RTC time\n");
    print("  sysinfo             - system and PC info\n");
    print("  mem                 - memory report\n");
    print("  pcinfo              - CPU vendor and memory info\n");
    print("  fs ...              - filesystem (ls/cat/touch/write/rm/save/load)\n");
    print("  zeron ...           - mini IDE/runner/compiler for Zeron tags\n");
    print("  color <fg> <bg>     - console colors (0..15)\n");
    print("  fs ...              - filesystem (ls/cat/touch/write/rm/save/load)\n");
    print("  ls/cat/touch/write  - short aliases for fs commands\n");
    print("  rm/save/load        - short aliases for fs commands\n");
    print("  install             - real ATA install marker write\n");
    print("  recovery            - recover important files\n");
    print("  echo <text>         - print text\n");
    print("  tester              - show tester list\n");
    print("  about               - about ZerDOS\n");
    print("  panic               - show GSOD\n");
}

static void cmd_sysinfo(void) {
    print("System: ZerDOS 0.2-dev\n");
    print("Kernel: 32-bit freestanding C + ASM\n");
    print("Display: VGA text 80x25 (white on green)\n");
    print("Filesystem: RAM FS + optional disk snapshot (LBA 2049)\n");
    print("Install mode: ATA PIO write + readback verify\n");
    print("UI: top status bar with date/time\n");
}

static void cmd_recovery(void) {
    recovery_mode = 0;
    if (!fs_find("system.cfg")) fs_add_file("system.cfg", "theme=green", 1);
    if (!fs_find("boot.log")) fs_add_file("boot.log", "Recovered boot log", 1);
    fs_sync_if_persistent();
    print("Recovery complete: important files checked/restored.\n");
}

static void cmd_tester(void) {
    print("ZerDOS testers:\n");
    print(" - Yarida\n");
    print(" - Vlad\n");
    print("Thanks for testing!\n");
}

static void cmd_about(void) {
    print("ZerDOS: hobby console OS project.\n");
    print("Goal: learn low-level dev and build custom shell features.\n");
}

static void prompt(void) {
    print("\nZerDOS> ");
}

static void boot_install_flow(void) {
    if (disk_has_install_marker("disk0")) {
        persistent_mode = 1;
        str_copy(active_disk_name, "disk0", sizeof(active_disk_name));
        print("ZerDOS installation detected on disk0.\n");
        if (fs_load_from_disk("disk0")) {
            print("Persistent filesystem loaded from disk0.\n");
        } else {
            print("No filesystem snapshot found on disk0, using defaults.\n");
            fs_init();
            fs_sync_if_persistent();
        }
        if (!fs_has_required_system_files()) {
            enter_recovery_mode("Required system files missing after boot load");
        }
        return;
    }

    print("ZERDOS was not found on disk.\n");
    print("Install it to disk? (y/n): ");
    while (1) {
        draw_status_bar();
        char c = get_keypress();
        if (c == 'y' || c == 'Y') {
            put_char(c);
            put_char('\n');
            if (do_real_install("disk0")) {
                persistent_mode = 1;
                str_copy(active_disk_name, "disk0", sizeof(active_disk_name));
                print("Install success on disk0.\n");
                if (fs_save_to_disk("disk0")) {
                    print("Filesystem snapshot initialized.\n");
                }
            } else {
                print("Install failed. Continuing in live mode.\n");
            }
            return;
        }
        if (c == 'n' || c == 'N') {
            put_char(c);
            put_char('\n');
            print("Skipped installation, running live mode.\n");
            return;
        }
    }
}

static int installer_wait_disk = 0;
static int installer_wait_confirm = 0;
static char installer_target[8];

static void run_command(const char* cmd) {
    if (installer_wait_disk) {
        if (streq(cmd, "disk0") || streq(cmd, "disk1") || streq(cmd, "disk2")) {
            str_copy(installer_target, cmd, sizeof(installer_target));
            installer_wait_disk = 0;
            installer_wait_confirm = 1;
            print("Target selected: ");
            print(installer_target);
            print("\nType YES to perform real install write: ");
        } else {
            print("Choose disk: disk0 / disk1 / disk2\n");
        }
        return;
    }
    if (installer_wait_confirm) {
        if (streq(cmd, "YES")) {
            print("Installing to ");
            print(installer_target);
            print(" ...\n");
            if (do_real_install(installer_target)) {
                persistent_mode = 1;
                str_copy(active_disk_name, installer_target, sizeof(active_disk_name));
                print("Install success: marker written at LBA 2048 and verified.\n");
                if (fs_save_to_disk(installer_target)) {
                    print("Filesystem snapshot saved at LBA 2049.\n");
                } else {
                    print("Warning: install marker saved, FS snapshot failed.\n");
                }
            } else {
                print("Install failed: ATA I/O error or disk unavailable.\n");
            }
            installer_wait_confirm = 0;
            installer_target[0] = '\0';
        } else {
            print("Install cancelled.\n");
            installer_wait_confirm = 0;
            installer_target[0] = '\0';
        }
        return;
    }

    if (streq(cmd, "help")) {
        cmd_help();
    } else if (streq(cmd, "clear")) {
        clear_screen();
        draw_status_bar();
    } else if (streq(cmd, "hello")) {
        print("Hello from ZerDOS!\n");
    } else if (streq(cmd, "time") || streq(cmd, "vrem")) {
        cmd_time();
    } else if (streq(cmd, "sysinfo")) {
        cmd_sysinfo();
    } else if (streq(cmd, "mem")) {
        cmd_mem();
    } else if (streq(cmd, "pcinfo")) {
        cmd_pcinfo();
    } else if (starts_with(cmd, "fs")) {
        if (cmd[2] == '\0') {
            cmd_fs("");
        } else if (cmd[2] == ' ') {
            cmd_fs(cmd + 3);
        } else {
            print("Unknown fs command\n");
        }
    } else if (streq(cmd, "ls")) {
        cmd_fs("ls");
    } else if (starts_with(cmd, "cat ")) {
        cmd_fs(cmd);
    } else if (starts_with(cmd, "touch ")) {
        cmd_fs(cmd);
    } else if (starts_with(cmd, "write ")) {
        cmd_fs(cmd);
    } else if (starts_with(cmd, "rm ")) {
        cmd_fs(cmd);
    } else if (streq(cmd, "save")) {
        cmd_fs("save");
    } else if (streq(cmd, "load")) {
        cmd_fs("load");
        if (!fs_has_required_system_files()) {
            enter_recovery_mode("Required system files missing after load");
        }
    } else if (starts_with(cmd, "zeron")) {
        if (cmd[5] == '\0') cmd_zeron("");
        else if (cmd[5] == ' ') cmd_zeron(cmd + 6);
        else print("Unknown zeron command\n");
    } else if (starts_with(cmd, "color")) {
        if (cmd[5] == '\0') cmd_color("");
        else if (cmd[5] == ' ') cmd_color(cmd + 6);
        else print("Usage: color <fg> <bg>\n");
    } else if (streq(cmd, "install")) {
        installer_wait_disk = 1;
        print("Installer started (real ATA mode).\n");
        print("Warning: this writes to disk sector LBA 2048.\n");
        print("Select target disk:\n");
        print(" - disk0\n - disk1\n - disk2\n");
    } else if (streq(cmd, "recovery")) {
        cmd_recovery();
    } else if (starts_with(cmd, "echo ")) {
        print(cmd + 5);
        print("\n");
    } else if (streq(cmd, "echo")) {
        print("Usage: echo <text>\n");
    } else if (streq(cmd, "tester")) {
        cmd_tester();
    } else if (streq(cmd, "about")) {
        cmd_about();
    } else if (streq(cmd, "panic")) {
        show_gsod("Manual panic command");
    } else if (cmd[0] != '\0') {
        print("Unknown command. Type help\n");
    }
}

void kmain(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    (void)multiboot_magic;

    if (multiboot_info_addr != 0) {
        struct multiboot_info* mbi = (struct multiboot_info*)(uintptr_t)multiboot_info_addr;
        if (mbi->flags & 0x1) {
            boot_mem_lower_kb = mbi->mem_lower;
            boot_mem_upper_kb = mbi->mem_upper;
        }
    }

    clear_screen();
    print("  ______              _____   ____   _____\n");
    print(" |___  /             |  __ \\ / __ \\ / ____|\n");
    print("    / /  ___ _ __ ___| |  | | |  | | (___\n");
    print("   / /  / _ \\ '__/ _ \\ |  | | |  | |\\___ \\\n");
    print("  / /__|  __/ | |  __/ |__| | |__| |____) |\n");
    print(" /_____\\___|_|  \\___|_____/ \\____/|_____/\n\n");

    fs_init();
    boot_install_flow();
    print("ASCII test ZerDOS\n");
    print("Type help and press Enter\n");

    char buffer[MAX_INPUT];
    size_t idx = 0;
    prompt();

    while (1) {
        draw_status_bar();
        char c = get_keypress();

        if (c == '\n') {
            put_char('\n');
            buffer[idx] = '\0';
            run_command(buffer);
            idx = 0;
            prompt();
            continue;
        }

        if (c == '\b') {
            if (idx > 0 && cursor_col > 0) {
                --idx;
                --cursor_col;
                vga[cursor_row * VGA_WIDTH + cursor_col] = ((uint16_t)current_color << 8) | ' ';
            }
            continue;
        }

        if (idx < sizeof(buffer) - 1) {
            buffer[idx++] = c;
            put_char(c);
        }
    }
}
