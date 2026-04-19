#include <stdint.h>
#include <stddef.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_ADDRESS 0xB8000

#define PORT_PS2_DATA 0x60
#define PORT_PS2_STATUS 0x64
#define PORT_PS2_CMD 0x64

#define COLOR(fg, bg) (uint8_t)(((bg) << 4) | ((fg) & 0x0F))

static volatile uint16_t* const vga = (uint16_t*)VGA_ADDRESS;

static int32_t cursor_x = 10;
static int32_t cursor_y = 8;
static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[3];

struct jpeg_info {
    uint16_t width;
    uint16_t height;
    uint8_t valid;
};

// testpaint (128x128 jpeg) placeholder blob with SOI/EOI and SOF0 size fields.
static const uint8_t testpaint[] = {
    0xFF, 0xD8,
    0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
    0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x80, 0x00, 0x80, 0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
    0xFF, 0xD9
};

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void io_wait(void) {
    outb(0x80, 0);
}

static void draw_cell(size_t x, size_t y, char ch, uint8_t color) {
    if (x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    vga[y * VGA_WIDTH + x] = ((uint16_t)color << 8) | (uint8_t)ch;
}

static void draw_text(size_t x, size_t y, const char* text, uint8_t color) {
    size_t i = 0;
    while (text[i] && x + i < VGA_WIDTH) {
        draw_cell(x + i, y, text[i], color);
        ++i;
    }
}

static struct jpeg_info decode_jpeg_header(const uint8_t* data, size_t size) {
    struct jpeg_info info = {0, 0, 0};

    if (size < 4) return info;
    if (!(data[0] == 0xFF && data[1] == 0xD8)) return info;

    size_t i = 2;
    while (i + 8 < size) {
        if (data[i] != 0xFF) {
            ++i;
            continue;
        }

        uint8_t marker = data[i + 1];
        if (marker == 0xD9) break;

        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }

        if (i + 3 >= size) break;

        uint16_t seg_len = (uint16_t)((data[i + 2] << 8) | data[i + 3]);
        if (seg_len < 2 || (i + 2 + seg_len) > size) break;

        if (marker == 0xC0 || marker == 0xC2) {
            if (seg_len >= 7) {
                info.height = (uint16_t)((data[i + 5] << 8) | data[i + 6]);
                info.width = (uint16_t)((data[i + 7] << 8) | data[i + 8]);
                info.valid = 1;
                return info;
            }
        }

        i += (size_t)(2 + seg_len);
    }

    return info;
}

static void draw_testpaint_texture(void) {
    struct jpeg_info info = decode_jpeg_header(testpaint, sizeof(testpaint));

    const int32_t start_x = 48;
    const int32_t start_y = 6;
    const int32_t w = 16;
    const int32_t h = 16;

    uint8_t ok_color = COLOR(15, 1);
    uint8_t fail_color = COLOR(15, 4);

    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            uint8_t c = ((x + y) & 1) ? ok_color : COLOR(14, 9);
            if (!info.valid || info.width != 128 || info.height != 128) {
                c = fail_color;
            }
            draw_cell((size_t)(start_x + x), (size_t)(start_y + y), ' ', c);
        }
    }

    if (info.valid && info.width == 128 && info.height == 128) {
        draw_text(44, 4, "testpaint JPEG 128x128", COLOR(15, 4));
    } else {
        draw_text(44, 4, "testpaint JPEG decode FAIL", COLOR(15, 4));
    }
}

static void draw_desktop(void) {
    const uint8_t desktop_color = COLOR(15, 4);

    for (size_t y = 0; y < VGA_HEIGHT; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            draw_cell(x, y, ' ', desktop_color);
        }
    }

    // Blue texture test square
    const uint8_t texture_color = COLOR(15, 1);
    for (size_t y = 6; y < 14; ++y) {
        for (size_t x = 28; x < 40; ++x) {
            draw_cell(x, y, ' ', texture_color);
        }
    }

    draw_text(2, 1, "ZerDOS GUI shell test", COLOR(15, 4));
    draw_text(2, 3, "Mouse: move cursor | W/A/S/D also works", COLOR(14, 4));
    draw_text(2, 4, "Q = reset cursor", COLOR(14, 4));

    draw_testpaint_texture();
}

static void draw_cursor(void) {
    static const uint8_t mask[3][3] = {
        {1, 0, 0},
        {1, 1, 0},
        {1, 1, 1}
    };

    for (int32_t y = 0; y < 3; ++y) {
        for (int32_t x = 0; x < 3; ++x) {
            if (!mask[y][x]) continue;
            int32_t px = cursor_x + x;
            int32_t py = cursor_y + y;
            if (px < 0 || py < 0 || px >= VGA_WIDTH || py >= VGA_HEIGHT) continue;
            draw_cell((size_t)px, (size_t)py, (char)0xDB, COLOR(0, 15));
        }
    }
}

static void render(void) {
    draw_desktop();
    draw_cursor();
}

static char scancode_to_ascii(uint8_t sc) {
    static const char map[128] = {
        [0x1E] = 'a', [0x11] = 'w', [0x1F] = 's', [0x20] = 'd',
        [0x10] = 'q'
    };

    if (sc >= 128) return 0;
    return map[sc];
}

static void clamp_cursor(void) {
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_x > (VGA_WIDTH - 3)) cursor_x = (VGA_WIDTH - 3);
    if (cursor_y > (VGA_HEIGHT - 3)) cursor_y = (VGA_HEIGHT - 3);
}

static void ps2_wait_write_ready(void) {
    while (inb(PORT_PS2_STATUS) & 0x02) {
    }
}

static void mouse_write(uint8_t value) {
    ps2_wait_write_ready();
    outb(PORT_PS2_CMD, 0xD4);
    ps2_wait_write_ready();
    outb(PORT_PS2_DATA, value);
}

static uint8_t mouse_read(void) {
    while (!(inb(PORT_PS2_STATUS) & 0x01)) {
    }
    return inb(PORT_PS2_DATA);
}

static void init_mouse(void) {
    ps2_wait_write_ready();
    outb(PORT_PS2_CMD, 0xA8);

    ps2_wait_write_ready();
    outb(PORT_PS2_CMD, 0x20);
    uint8_t status = mouse_read();
    status |= 0x02;
    ps2_wait_write_ready();
    outb(PORT_PS2_CMD, 0x60);
    ps2_wait_write_ready();
    outb(PORT_PS2_DATA, status);

    mouse_write(0xF6);
    (void)mouse_read();
    mouse_write(0xF4);
    (void)mouse_read();
}

static void handle_mouse_byte(uint8_t b) {
    if (mouse_cycle == 0 && (b & 0x08) == 0) {
        return;
    }

    mouse_packet[mouse_cycle++] = b;
    if (mouse_cycle < 3) return;
    mouse_cycle = 0;

    int32_t dx = (int8_t)mouse_packet[1];
    int32_t dy = (int8_t)mouse_packet[2];

    cursor_x += dx;
    cursor_y -= dy;
    clamp_cursor();
}

static void process_input(void) {
    while (inb(PORT_PS2_STATUS) & 0x01) {
        uint8_t status = inb(PORT_PS2_STATUS);
        uint8_t data = inb(PORT_PS2_DATA);

        if (status & 0x20) {
            handle_mouse_byte(data);
            continue;
        }

        if (data & 0x80) continue;
        char key = scancode_to_ascii(data);
        if (key == 'w') cursor_y -= 1;
        else if (key == 's') cursor_y += 1;
        else if (key == 'a') cursor_x -= 1;
        else if (key == 'd') cursor_x += 1;
        else if (key == 'q') {
            cursor_x = 10;
            cursor_y = 8;
        }
        clamp_cursor();
    }
}

void kmain(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    (void)multiboot_magic;
    (void)multiboot_info_addr;

    init_mouse();
    render();

    while (1) {
        process_input();
        render();
        io_wait();
    }
}
