#include <cstdio>
#include <cstring>
#include <hardware/flash.h>
#include <hardware/vreg.h>
#include <hardware/watchdog.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>

extern "C" {
#include <pce-go/pce.h>
#include <pce-go/psg.h>
}

#include <graphics.h>
#include "audio.h"

#include "nespad.h"
#include "ff.h"
#include "ps2kbd_mrmltr.h"

#define HOME_DIR "\\PCE"
extern char __flash_binary_end;
#define FLASH_TARGET_OFFSET (((((uintptr_t)&__flash_binary_end - XIP_BASE) / FLASH_SECTOR_SIZE) + 4) * FLASH_SECTOR_SIZE)
static const uintptr_t rom = XIP_BASE + FLASH_TARGET_OFFSET;

#define AUDIO_SAMPLE_RATE 22050
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60 + 1)

char __uninitialized_ram(filename[256]);
static uint32_t __uninitialized_ram(rom_size);

static FATFS fs;
bool reboot = false;
semaphore vga_start_semaphore;

alignas(4) uint8_t SCREEN[XBUF_HEIGHT][XBUF_WIDTH];
alignas(4) int audio_buffer[AUDIO_BUFFER_LENGTH];

struct input_bits_t {
    bool a: true;
    bool b: true;
    bool select: true;
    bool start: true;
    bool right: true;
    bool left: true;
    bool up: true;
    bool down: true;
};

static input_bits_t keyboard_bits = { false, false, false, false, false, false, false, false };
static input_bits_t gamepad1_bits = { false, false, false, false, false, false, false, false };
static input_bits_t gamepad2_bits = { false, false, false, false, false, false, false, false };
static uint8_t fxPressedV = 0;
static bool swap_ab = false;
static bool ctrlPressed = false;
static bool altPressed = false;

static void load_config() {
    char pathname[256];
    sprintf(pathname, "%s\\pico-pce.cfg", HOME_DIR);
    FIL fd;
    FRESULT fr = f_open(&fd, pathname, FA_READ);
    if (fr != FR_OK) {
        return;
    }
    UINT br;
    f_read(&fd, &swap_ab, sizeof(swap_ab), &br);
#if SOFTTV
    f_read(&fd, &tv_out_mode, sizeof(tv_out_mode), &br);
#endif
    f_close(&fd);
}

static void save_config() {
    char pathname[256];
    sprintf(pathname, "%s\\pico-pce.cfg", HOME_DIR);
    FIL fd;
    FRESULT fr = f_open(&fd, pathname, FA_CREATE_ALWAYS | FA_WRITE);
    UINT bw;
    f_write(&fd, &swap_ab, sizeof(swap_ab), &bw);
#if SOFTTV
    f_write(&fd, &tv_out_mode, sizeof(tv_out_mode), &bw);
#endif
    f_close(&fd);
}

static void nespad_tick() {
    nespad_read();

    uint8_t controls_state = 0;

    if (swap_ab) {
        gamepad1_bits.b = (nespad_state & DPAD_A) != 0;
        gamepad1_bits.a = (nespad_state & DPAD_B) != 0;
    } else {
        gamepad1_bits.a = (nespad_state & DPAD_A) != 0;
        gamepad1_bits.b = (nespad_state & DPAD_B) != 0;

    }

    gamepad1_bits.select = (nespad_state & DPAD_SELECT) != 0;
    gamepad1_bits.start = (nespad_state & DPAD_START) != 0;
    gamepad1_bits.up = (nespad_state & DPAD_UP) != 0;
    gamepad1_bits.down = (nespad_state & DPAD_DOWN) != 0;
    gamepad1_bits.left = (nespad_state & DPAD_LEFT) != 0;
    gamepad1_bits.right = (nespad_state & DPAD_RIGHT) != 0;


    if (gamepad1_bits.up) controls_state |= 0x08;
    if (gamepad1_bits.down) controls_state |= 0x04;
    if (gamepad1_bits.left) controls_state |= 0x02;
    if (gamepad1_bits.right) controls_state |= 0x01;
    if (gamepad1_bits.a) controls_state |= 0x20;
    if (gamepad1_bits.b) controls_state |= 0x10;
    if (gamepad1_bits.start) controls_state |= 0x80;
    if (gamepad1_bits.select) controls_state |= 0x40;
}

static bool isInReport(hid_keyboard_report_t const* report, const unsigned char keycode) {
    for (unsigned char i: report->keycode) {
        if (i == keycode) {
            return true;
        }
    }
    return false;
}

void
__not_in_flash_func(process_kbd_report)(hid_keyboard_report_t const* report, hid_keyboard_report_t const* prev_report) {
    /* printf("HID key report modifiers %2.2X report ", report->modifier);
    for (unsigned char i: report->keycode)
        printf("%2.2X", i);
    printf("\r\n");
     */
    keyboard_bits.start = isInReport(report, HID_KEY_ENTER) || isInReport(report, HID_KEY_KEYPAD_ENTER);
    keyboard_bits.select = isInReport(report, HID_KEY_BACKSPACE) || isInReport(report, HID_KEY_ESCAPE) || isInReport(report, HID_KEY_KEYPAD_ADD);

    if (swap_ab) {
        keyboard_bits.b = isInReport(report, HID_KEY_Z) || isInReport(report, HID_KEY_O) || isInReport(report, HID_KEY_K) || isInReport(report, HID_KEY_KEYPAD_0);
        keyboard_bits.a = isInReport(report, HID_KEY_X) || isInReport(report, HID_KEY_P) || isInReport(report, HID_KEY_L) || isInReport(report, HID_KEY_KEYPAD_DECIMAL);
    } else {
        keyboard_bits.a = isInReport(report, HID_KEY_Z) || isInReport(report, HID_KEY_O) || isInReport(report, HID_KEY_K) || isInReport(report, HID_KEY_KEYPAD_0);
        keyboard_bits.b = isInReport(report, HID_KEY_X) || isInReport(report, HID_KEY_P) || isInReport(report, HID_KEY_L) || isInReport(report, HID_KEY_KEYPAD_DECIMAL);
    }


    bool b7 = isInReport(report, HID_KEY_KEYPAD_7);
    bool b9 = isInReport(report, HID_KEY_KEYPAD_9);
    bool b1 = isInReport(report, HID_KEY_KEYPAD_1);
    bool b3 = isInReport(report, HID_KEY_KEYPAD_3);

    keyboard_bits.up = b7 || b9 || isInReport(report, HID_KEY_ARROW_UP) || isInReport(report, HID_KEY_W) || isInReport(report, HID_KEY_KEYPAD_8);
    keyboard_bits.down = b1 || b3 || isInReport(report, HID_KEY_ARROW_DOWN) || isInReport(report, HID_KEY_S) || isInReport(report, HID_KEY_KEYPAD_5) || isInReport(report, HID_KEY_KEYPAD_2);
    keyboard_bits.left = b7 || b1 || isInReport(report, HID_KEY_ARROW_LEFT) || isInReport(report, HID_KEY_A) || isInReport(report, HID_KEY_KEYPAD_4);
    keyboard_bits.right = b9 || b3 || isInReport(report, HID_KEY_ARROW_RIGHT)  || isInReport(report, HID_KEY_D) || isInReport(report, HID_KEY_KEYPAD_6);
    
    altPressed = isInReport(report, HID_KEY_ALT_LEFT) || isInReport(report, HID_KEY_ALT_RIGHT);
    ctrlPressed = isInReport(report, HID_KEY_CONTROL_LEFT) || isInReport(report, HID_KEY_CONTROL_RIGHT);
    if (altPressed && ctrlPressed && isInReport(report, HID_KEY_DELETE)) {
        watchdog_enable(10, true);
        while(true) {
            tight_loop_contents();
        }
    }    
    if (ctrlPressed || altPressed) {
        uint8_t fxPressed = 0;
        if (isInReport(report, HID_KEY_F1)) fxPressed = 1;
        else if (isInReport(report, HID_KEY_F2)) fxPressed = 2;
        else if (isInReport(report, HID_KEY_F3)) fxPressed = 3;
        else if (isInReport(report, HID_KEY_F4)) fxPressed = 4;
        else if (isInReport(report, HID_KEY_F5)) fxPressed = 5;
        else if (isInReport(report, HID_KEY_F6)) fxPressed = 6;
        else if (isInReport(report, HID_KEY_F7)) fxPressed = 7;
        else if (isInReport(report, HID_KEY_F8)) fxPressed = 8;
        fxPressedV = fxPressed;
    }
}

Ps2Kbd_Mrmltr ps2kbd(
        pio1,
        0,
        process_kbd_report
);

i2s_config_t i2s_config;

typedef struct __attribute__((__packed__)) {
    bool is_directory;
    bool is_executable;
    size_t size;
    char filename[79];
} file_item_t;

constexpr int max_files = 600;
file_item_t *fileItems = (file_item_t *) (&SCREEN[0][0] + TEXTMODE_COLS * TEXTMODE_ROWS * 2);

int compareFileItems(const void *a, const void *b) {
    const auto *itemA = (file_item_t *) a;
    const auto *itemB = (file_item_t *) b;
    // Directories come first
    if (itemA->is_directory && !itemB->is_directory)
        return -1;
    if (!itemA->is_directory && itemB->is_directory)
        return 1;
    // Sort files alphabetically
    return strcmp(itemA->filename, itemB->filename);
}

bool isExecutable(const char pathname[255], const char *extensions) {
    char *pathCopy = strdup(pathname);
    const char *token = strrchr(pathCopy, '.');

    if (token == nullptr) {
        return false;
    }

    token++;

    while (token != NULL) {
        if (strstr(extensions, token) != NULL) {
            free(pathCopy);
            return true;
        }
        token = strtok(NULL, ",");
    }
    free(pathCopy);
    return false;
}

bool filebrowser_loadfile(const char pathname[256]) {
    UINT bytes_read = 0;
    FIL file;

    constexpr int window_y = (TEXTMODE_ROWS - 5) / 2;
    constexpr int window_x = (TEXTMODE_COLS - 43) / 2;

    draw_window("Loading ROM", window_x, window_y, 43, 5);

    FILINFO fileinfo;
    f_stat(pathname, &fileinfo);
    rom_size = fileinfo.fsize;
    if (16384 - 64 << 10 < fileinfo.fsize) {
        draw_text("ERROR: ROM too large! Canceled!!", window_x + 1, window_y + 2, 13, 1);
        sleep_ms(5000);
        return false;
    }

    draw_text("Loading...", window_x + 1, window_y + 2, 10, 1);
    sleep_ms(500);

    multicore_lockout_start_blocking();
    auto flash_target_offset = FLASH_TARGET_OFFSET;
    const uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(flash_target_offset, fileinfo.fsize);
    restore_interrupts(ints);

    if (FR_OK == f_open(&file, pathname, FA_READ)) {
        uint8_t buffer[FLASH_PAGE_SIZE];

        do {
            f_read(&file, &buffer, FLASH_PAGE_SIZE, &bytes_read);

            if (bytes_read) {
                const uint32_t ints = save_and_disable_interrupts();
                flash_range_program(flash_target_offset, buffer, FLASH_PAGE_SIZE);
                restore_interrupts(ints);

                gpio_put(PICO_DEFAULT_LED_PIN, flash_target_offset >> 13 & 1);

                flash_target_offset += FLASH_PAGE_SIZE;
            }
        } while (bytes_read != 0);

        gpio_put(PICO_DEFAULT_LED_PIN, true);
    }
    f_close(&file);
    multicore_lockout_end_blocking();
    // restore_interrupts(ints);

    strcpy(filename, fileinfo.fname);

    return true;
}

void filebrowser(const char pathname[256], const char executables[11]) {
    bool debounce = true;
    char basepath[256];
    char tmp[TEXTMODE_COLS + 1];
    strcpy(basepath, pathname);
    constexpr int per_page = TEXTMODE_ROWS - 3;

    DIR dir;
    FILINFO fileInfo;

    while (true) {
        memset(fileItems, 0, sizeof(file_item_t) * max_files);
        int total_files = 0;

        snprintf(tmp, TEXTMODE_COLS, "SD:\\%s", basepath);
        draw_window(tmp, 0, 0, TEXTMODE_COLS, TEXTMODE_ROWS - 1);
        memset(tmp, ' ', TEXTMODE_COLS);


        draw_text(tmp, 0, 29, 0, 0);
        auto off = 0;
        draw_text("START", off, 29, 7, 0);
        off += 5;
        draw_text(" Run at cursor ", off, 29, 0, 3);
        off += 16;
        draw_text("SELECT", off, 29, 7, 0);
        off += 6;
        draw_text(" Run previous  ", off, 29, 0, 3);
#ifndef TFT
        off += 16;
        draw_text("ARROWS", off, 29, 7, 0);
        off += 6;
        draw_text(" Navigation    ", off, 29, 0, 3);
        off += 16;
        draw_text("A/F10", off, 29, 7, 0);
        off += 5;
        draw_text(" USB DRV ", off, 29, 0, 3);
#endif

        if (FR_OK != f_opendir(&dir, basepath)) {
            draw_text("Failed to open directory", 1, 1, 4, 0);
            while (true);
        }

        if (strlen(basepath) > 0) {
            strcpy(fileItems[total_files].filename, "..\0");
            fileItems[total_files].is_directory = true;
            fileItems[total_files].size = 0;
            total_files++;
        }

        while (f_readdir(&dir, &fileInfo) == FR_OK &&
               fileInfo.fname[0] != '\0' &&
               total_files < max_files
                ) {
            // Set the file item properties
            fileItems[total_files].is_directory = fileInfo.fattrib & AM_DIR;
            fileItems[total_files].size = fileInfo.fsize;
            fileItems[total_files].is_executable = isExecutable(fileInfo.fname, executables);
            strncpy(fileItems[total_files].filename, fileInfo.fname, 78);
            total_files++;
        }
        f_closedir(&dir);

        qsort(fileItems, total_files, sizeof(file_item_t), compareFileItems);

        if (total_files > max_files) {
            draw_text(" Too many files!! ", TEXTMODE_COLS - 17, 0, 12, 3);
        }

        int offset = 0;
        int current_item = 0;

        while (true) {
            sleep_ms(100);

            if (!debounce) {
                debounce = !(nespad_state & DPAD_START || keyboard_bits.start);
            }

            // ESCAPE
            if (nespad_state & DPAD_SELECT || keyboard_bits.select) {
                return;
            }

            if (nespad_state & DPAD_DOWN || keyboard_bits.down) {
                if (offset + (current_item + 1) < total_files) {
                    if (current_item + 1 < per_page) {
                        current_item++;
                    } else {
                        offset++;
                    }
                }
            }

            if (nespad_state & DPAD_UP || keyboard_bits.up) {
                if (current_item > 0) {
                    current_item--;
                } else if (offset > 0) {
                    offset--;
                }
            }

            if (nespad_state & DPAD_RIGHT || keyboard_bits.right) {
                offset += per_page;
                if (offset + (current_item + 1) > total_files) {
                    offset = total_files - (current_item + 1);
                }
            }

            if (nespad_state & DPAD_LEFT || keyboard_bits.left) {
                if (offset > per_page) {
                    offset -= per_page;
                } else {
                    offset = 0;
                    current_item = 0;
                }
            }

            if (debounce && (nespad_state & DPAD_START || keyboard_bits.start)) {
                auto file_at_cursor = fileItems[offset + current_item];

                if (file_at_cursor.is_directory) {
                    if (strcmp(file_at_cursor.filename, "..") == 0) {
                        const char *lastBackslash = strrchr(basepath, '\\');
                        if (lastBackslash != nullptr) {
                            const size_t length = lastBackslash - basepath;
                            basepath[length] = '\0';
                        }
                    } else {
                        sprintf(basepath, "%s\\%s", basepath, file_at_cursor.filename);
                    }
                    debounce = false;
                    break;
                }

                if (file_at_cursor.is_executable) {
                    sprintf(tmp, "%s\\%s", basepath, file_at_cursor.filename);

                    filebrowser_loadfile(tmp);
                    return;
                }
            }

            for (int i = 0; i < per_page; i++) {
                uint8_t color = 11;
                uint8_t bg_color = 1;

                if (offset + i < max_files) {
                    const auto item = fileItems[offset + i];


                    if (i == current_item) {
                        color = 0;
                        bg_color = 3;
                        memset(tmp, 0xCD, TEXTMODE_COLS - 2);
                        tmp[TEXTMODE_COLS - 2] = '\0';
                        draw_text(tmp, 1, per_page + 1, 11, 1);
                        snprintf(tmp, TEXTMODE_COLS - 2, " Size: %iKb, File %lu of %i ", item.size / 1024,
                                 offset + i + 1,
                                 total_files);
                        draw_text(tmp, 2, per_page + 1, 14, 3);
                    }

                    const auto len = strlen(item.filename);
                    color = item.is_directory ? 15 : color;
                    color = item.is_executable ? 10 : color;
                    //color = strstr((char *)rom_filename, item.filename) != nullptr ? 13 : color;

                    memset(tmp, ' ', TEXTMODE_COLS - 2);
                    tmp[TEXTMODE_COLS - 2] = '\0';
                    memcpy(&tmp, item.filename, len < TEXTMODE_COLS - 2 ? len : TEXTMODE_COLS - 2);
                } else {
                    memset(tmp, ' ', TEXTMODE_COLS - 2);
                }
                draw_text(tmp, 1, i + 1, color, bg_color);
            }
        }
    }
}

enum menu_type_e {
    NONE,
    INT,
    TEXT,
    ARRAY,

    SAVE,
    LOAD,
    ROM_SELECT,
    RETURN,
};

typedef bool (*menu_callback_t)();

typedef struct __attribute__((__packed__)) {
    const char *text;
    menu_type_e type;
    const void *value;
    menu_callback_t callback;
    uint8_t max_value;
    char value_list[15][10];
} MenuItem;

int save_slot = 0;
uint16_t frequencies[] = { CPU_FREQ, 396, 404, 408, 412, 416, 420, 424, 432 };
uint8_t frequency_index = 0;

bool overclock() {
#if PICO_RP2350
    volatile uint32_t *qmi_m0_timing=(uint32_t *)0x400d000c;
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
    sleep_ms(10);
    *qmi_m0_timing = 0x60007204;
    set_sys_clock_khz(frequencies[frequency_index] * KHZ, false);
    *qmi_m0_timing = 0x60007303;
    return true;
#else
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(33);
    return set_sys_clock_khz(frequencies[frequency_index] * KHZ, true);
#endif
}

bool save() {
    char pathname[255];
    if (save_slot) {
        sprintf(pathname, "%s\\%s_%d.save", HOME_DIR, filename, save_slot);
    } else {
        sprintf(pathname, "%s\\%s.save", HOME_DIR, filename);
    }
    SaveState(pathname);
    return true;
}

bool load() {
    char pathname[255];
    if (save_slot) {
        sprintf(pathname, "%s\\%s_%d.save", HOME_DIR, filename, save_slot);
    } else {
        sprintf(pathname, "%s\\%s.save", HOME_DIR, filename);
    }
    return LoadState(pathname) > -1;
}
#if SOFTTV
typedef struct tv_out_mode_t {
    // double color_freq;
    float color_index;
    COLOR_FREQ_t c_freq;
    enum graphics_mode_t mode_bpp;
    g_out_TV_t tv_system;
    NUM_TV_LINES_t N_lines;
    bool cb_sync_PI_shift_lines;
    bool cb_sync_PI_shift_half_frame;
} tv_out_mode_t;
extern tv_out_mode_t tv_out_mode;

bool color_mode=true;
bool toggle_color() {
    color_mode=!color_mode;
    if(color_mode) {
        tv_out_mode.color_index= 1.0f;
    } else {
        tv_out_mode.color_index= 0.0f;
    }

    return true;
}
#endif

const MenuItem menu_items[] = {
        { "Swap AB <> BA: %s", ARRAY, &swap_ab, nullptr, 1, { "NO ", "YES" }},
        {},
        //{ "Player 1: %s",        ARRAY, &player_1_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
        //{ "Player 2: %s",        ARRAY, &player_2_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
        { "Save state: %i", INT, &save_slot, &save, 5 },
        { "Load state: %i", INT, &save_slot, &load, 5 },
        {},
#if SOFTTV
        { "TV system %s", ARRAY, &tv_out_mode.tv_system, nullptr, 1, { "PAL ", "NTSC" } },
        { "TV Lines %s", ARRAY, &tv_out_mode.N_lines, nullptr, 3, { "624", "625", "524", "525" } },
        { "Freq %s", ARRAY, &tv_out_mode.c_freq, nullptr, 1, { "3.579545", "4.433619" } },
        { "Colors: %s", ARRAY, &color_mode, &toggle_color, 1, { "NO ", "YES" } },
        { "Shift lines %s", ARRAY, &tv_out_mode.cb_sync_PI_shift_lines, nullptr, 1, { "NO ", "YES" } },
        { "Shift half frame %s", ARRAY, &tv_out_mode.cb_sync_PI_shift_half_frame, nullptr, 1, { "NO ", "YES" } },
#endif
        {
                "Overclocking: %s MHz", ARRAY, &frequency_index, &overclock, count_of(frequencies) - 1,
                { "378", "396", "404", "408", "412", "416", "420", "424", "432" }
        },
        { "Press START / Enter to apply", NONE },
        { "Reset to ROM select", ROM_SELECT },
        { "Return to game", RETURN }
};
#define MENU_ITEMS_NUMBER (sizeof(menu_items) / sizeof (MenuItem))

void menu() {
    bool exit = false;
    graphics_set_mode(TEXTMODE_DEFAULT);
    char footer[TEXTMODE_COLS];
    snprintf(footer, TEXTMODE_COLS, ":: %s ::", PICO_PROGRAM_NAME);
    draw_text(footer, TEXTMODE_COLS / 2 - strlen(footer) / 2, 0, 11, 1);
    snprintf(footer, TEXTMODE_COLS, ":: %s build %s %s ::", PICO_PROGRAM_VERSION_STRING, __DATE__,
             __TIME__);
    draw_text(footer, TEXTMODE_COLS / 2 - strlen(footer) / 2, TEXTMODE_ROWS - 1, 11, 1);
    uint current_item = 0;

    while (!exit) {
        for (int i = 0; i < MENU_ITEMS_NUMBER; i++) {
            uint8_t y = i + (TEXTMODE_ROWS - MENU_ITEMS_NUMBER >> 1);
            uint8_t x = TEXTMODE_COLS / 2 - 10;
            uint8_t color = 0xFF;
            uint8_t bg_color = 0x00;
            if (current_item == i) {
                color = 0x01;
                bg_color = 0xFF;
            }
            const MenuItem *item = &menu_items[i];
            if (i == current_item) {
                switch (item->type) {
                    case INT:
                    case ARRAY:
                        if (item->max_value != 0) {
                            auto *value = (uint8_t *) item->value;
                            if ((gamepad1_bits.right || keyboard_bits.right) && *value < item->max_value) {
                                (*value)++;
                            }
                            if ((gamepad1_bits.left || keyboard_bits.left) && *value > 0) {
                                (*value)--;
                            }
                        }
                        break;
                    case RETURN:
                        if (gamepad1_bits.start || keyboard_bits.start)
                            exit = true;
                        break;

                    case ROM_SELECT:
                        if (gamepad1_bits.start || keyboard_bits.start) {
                            reboot = true;
                            return;
                        }
                        break;
                    default:
                        break;
                }

                if (nullptr != item->callback && (gamepad1_bits.start || keyboard_bits.start)) {
                    exit = item->callback();
                }
            }
            static char result[TEXTMODE_COLS];
            switch (item->type) {
                case INT:
                    snprintf(result, TEXTMODE_COLS, item->text, *(uint8_t *) item->value);
                    break;
                case ARRAY:
                    snprintf(result, TEXTMODE_COLS, item->text, item->value_list[*(uint8_t *) item->value]);
                    break;
                case TEXT:
                    snprintf(result, TEXTMODE_COLS, item->text, item->value);
                    break;
                case NONE:
                    color = 6;
                default:
                    snprintf(result, TEXTMODE_COLS, "%s", item->text);
            }
            draw_text(result, x, y, color, bg_color);
        }

        if (gamepad1_bits.down || keyboard_bits.down) {
            current_item = (current_item + 1) % MENU_ITEMS_NUMBER;

            if (menu_items[current_item].type == NONE)
                current_item++;
        }
        if (gamepad1_bits.up || keyboard_bits.up) {
            current_item = (current_item - 1 + MENU_ITEMS_NUMBER) % MENU_ITEMS_NUMBER;

            if (menu_items[current_item].type == NONE)
                current_item--;
        }

        sleep_ms(125);
    }
    save_config();
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
}

/* Renderer loop on Pico's second core */
void __time_critical_func(render_core)() {
    multicore_lockout_victim_init();

    i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = AUDIO_SAMPLE_RATE / 60;
    i2s_volume(&i2s_config, 0);
    i2s_init(&i2s_config);

    ps2kbd.init_gpio();
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);

    graphics_init();

    const auto buffer = (uint8_t *) SCREEN;
    graphics_set_buffer(buffer, 256, 240);
    graphics_set_textbuffer(buffer);
    graphics_set_bgcolor(0x000000);

    graphics_set_offset(32,0);

    for (int i = 0; i < 256; i++) {
        graphics_set_palette(i, RGB888(
                                     (((i & 0x1C) >> 1) * 16),
                                     (((i & 0xe0) >> 4) * 16),
                                     (((i & 0x03) << 2) * 16)
                             )
        );
    }

    graphics_set_flashmode(true, true);
    sem_acquire_blocking(&vga_start_semaphore);

    // 60 FPS loop
#define frame_tick (16666)
    uint64_t tick = time_us_64();
    uint64_t last_frame_tick = tick;

    while (true) {

        if (tick >= last_frame_tick + frame_tick) {
#ifdef TFT
            refresh_lcd();
#endif
            ps2kbd.tick();
            nespad_tick();

            last_frame_tick = tick;
        }

        tick = time_us_64();

        // tuh_task();
        // hid_app_task();
        tight_loop_contents();
    }

    __unreachable();
}

int frame, frame_cnt = 0;
int frame_timer_start = 0;

int main() {
    overclock();

    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }

    if (FR_OK != f_mount(&fs, "SD", 1)) {
        draw_text("SD Card not inserted or SD Card error!", 0, 0, 12, 0);
        while (true);
    }
    f_mkdir(HOME_DIR);
    load_config();

    while (true) {
        graphics_set_mode(TEXTMODE_DEFAULT);
        filebrowser(HOME_DIR, "pce");
        InitPCE(AUDIO_SAMPLE_RATE, true, (uint8_t *) rom, rom_size);
        graphics_set_mode(GRAPHICSMODE_DEFAULT);

        frame = 0;
        while (!reboot) {
            uint32_t buttons = 0;
            if (gamepad1_bits.left || keyboard_bits.left) buttons |= JOY_LEFT;
            if (gamepad1_bits.right || keyboard_bits.right) buttons |= JOY_RIGHT;
            if (gamepad1_bits.up || keyboard_bits.up) buttons |= JOY_UP;
            if (gamepad1_bits.down || keyboard_bits.down) buttons |= JOY_DOWN;
            if (gamepad1_bits.a || keyboard_bits.a) buttons |= JOY_A;
            if (gamepad1_bits.b || keyboard_bits.b) buttons |= JOY_B;
            if (gamepad1_bits.start || keyboard_bits.start) buttons |= JOY_RUN;
            if (gamepad1_bits.select || keyboard_bits.select) buttons |= JOY_SELECT;
            PCE.Joypad.regs[0] = buttons;

            if ((gamepad1_bits.start && gamepad1_bits.select) || (keyboard_bits.start && keyboard_bits.select)) {
                menu();
            }

            pce_run();

            graphics_set_buffer((uint8_t*)SCREEN, PCE.VDC.screen_width == 256 ? 256 : 320, PCE.VDC.screen_height);
            graphics_set_offset(PCE.VDC.screen_width == 256 ? 32 : 0,0);

            psg_update((int16_t *) audio_buffer, AUDIO_BUFFER_LENGTH, 0xff);
            i2s_dma_write(&i2s_config, (const int16_t *) audio_buffer);

            frame++;
            if (0) {
                if (++frame_cnt == 6) {
                    while (time_us_64() - frame_timer_start < 16666 * 6);  // 60 Hz
                    frame_timer_start = time_us_64();
                    frame_cnt = 0;
                }
            }
            if (fxPressedV) {
                save_slot = fxPressedV;
                if (altPressed) {
                    load();
                }
                if (ctrlPressed) {
                    save();
                }
                fxPressedV = 0;
            }

            tight_loop_contents();
        }

        reboot = false;
    }
    __unreachable();
}
