#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "framebuffer.h"

int create_fb(struct framebuffer** framebuffer, uint16_t width, uint16_t height, char* shared_memory_name) {
    int fd = shm_open(shared_memory_name, O_CREAT | O_RDWR, 0666);
    if(fd == -1) {
        printf("Failed to create shared memory with name %s: %s\n", shared_memory_name, strerror(errno));
        return errno;
    }

    struct stat shared_memory_stats;
    if (fstat(fd, &shared_memory_stats) == -1) {
        printf("Failed to fstat the shared memory with name %s: %s\n", shared_memory_name, strerror(errno));
        return errno;
    }

    int expected_shared_memory_size = 2 * sizeof(uint16_t) /* size header */
        + width * height * sizeof(uint32_t) /* pixels */
        + MAX_PORTS * sizeof(struct port_stats) /* statistics for every per port */;

    bool fresh_shm = false;
    if (shared_memory_stats.st_size == 0) {
        // Shared memory was freshly created (with size 0), we need to initialize it
        fresh_shm = true;

        if (ftruncate(fd, expected_shared_memory_size) == -1) {
            printf("Failed to resize the shared memory with name %s to size of %u bytes: %s\n",
                shared_memory_name, expected_shared_memory_size, strerror(errno));
            return errno;
        }
    } else if (shared_memory_stats.st_size != expected_shared_memory_size) {
        printf("Found existing shared memory with size of %lu bytes. However, I expected it to be of size %u, as the"
            "framebuffer has (%u, %u) pixels. The Pixelflut backend and frontend seem to use different resolutions! "
            "In case you want to re-size your existing framebuffer please execute 'rm /dev/shm%s'\n",
            shared_memory_stats.st_size, expected_shared_memory_size, width, height, shared_memory_name);
        return EINVAL;
    } else {
        printf("Using existing shared memory of correct size\n");
    }

    char* shared_memory;
    shared_memory = mmap(NULL, expected_shared_memory_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared_memory == MAP_FAILED) {
        printf("Failed to mmap the the shared memory with name %s: %s\n", shared_memory_name, strerror(errno));
        return errno;
    }

    // Zero the new shared memory, as e.g. the statistics rely on the fact that the mac addresses initialize with zero.
    if (fresh_shm) {
        memset(shared_memory, 0, expected_shared_memory_size);
    }

    // We need to set width and height so that other tools (e.g. the frontend) can detect the framebuffer size
    uint16_t* width_ptr = (uint16_t*)shared_memory;
    if (*width_ptr == 0) {
        // Shared memory was freshly created, we need to initialize it
        *width_ptr = width;
    } else if (*width_ptr != width) {
        printf("Found existing shared memory, but it has the width %u, while I expected %u\n", *width_ptr, width);
        return EINVAL;
    }

    uint16_t* height_ptr = (uint16_t*)(shared_memory + 2);
    if (*height_ptr == 0) {
        // Shared memory was freshly created, we need to initialize it
        *height_ptr = height;
    } else if (*height_ptr != height) {
        printf("Found existing shared memory, but it has the height %u, while I expected %u\n", *height_ptr, height);
        return EINVAL;
    }

    struct framebuffer* fb = malloc(sizeof(struct framebuffer));
    fb->width = width;
    fb->height = height;
    fb->pixels = (uint32_t*)(shared_memory + 2 * sizeof(uint16_t) /* size header */);
    fb->port_stats = (struct port_stats*)(shared_memory + 2 * sizeof(uint16_t) /* size header */ + width * height * sizeof(uint32_t) /* pixels */);

    printf("Created framebuffer of size (%u,%u) backed by shared memory with the name %s\n",
        width, height, shared_memory_name);

    *framebuffer = fb;
    return 0;
}

// Only sets pixel if it is within bounds
void fb_set(struct framebuffer* framebuffer, uint16_t x, uint16_t y, uint32_t rgba) {
    if (x < framebuffer->width && y < framebuffer->height) {
        framebuffer->pixels[x + y * framebuffer->width] = rgba;
    }
}

// Does *not* check for bounds
uint32_t fb_get(struct framebuffer* framebuffer, uint16_t x, uint16_t y) {
    return framebuffer->pixels[x + y * framebuffer->width];
}
