#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include "gb.h"
#include "ws_server.h"
#include "constants.h"

void server_main_impl(void) {
    int frame_num = 0;
    while (!g_shutdown_requested) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        frame_headless();
        ws_server_notify_frame(pixels, VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
        ws_server_service();

        if (++frame_num % 300 == 0 && ext_ram_dirty && savestate_rom_path[0] != '\0')
            sav_save(savestate_rom_path);

        clock_gettime(CLOCK_MONOTONIC, &t1);
        long elapsed_us = (t1.tv_sec - t0.tv_sec) * 1000000L
                        + (t1.tv_nsec - t0.tv_nsec) / 1000L;
        long target_us = 16667L;
        if (elapsed_us < target_us) {
            long sleep_us = target_us - elapsed_us;
            if (g_fast_mode) sleep_us /= 4;
            usleep((unsigned int)sleep_us);
        }
    }
}
