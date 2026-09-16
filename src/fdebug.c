#define _GNU_SOURCE
#include "fdebug.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* From raspberrypi/utils piolib/include/rp1_pio_if.h (not installed by piolib). */
struct rp1_access_hw_args {
    uint32_t addr;
    uint32_t len;
    void *data;
};
#define PIO_IOC_MAGIC 102
#define PIO_IOC_READ_HW _IOW(PIO_IOC_MAGIC, 8, struct rp1_access_hw_args)
#define PIO_IOC_WRITE_HW _IOW(PIO_IOC_MAGIC, 9, struct rp1_access_hw_args)

int fdebug_open(void)
{
    return open("/dev/pio0", O_RDWR | O_CLOEXEC);
}

void fdebug_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

int fdebug_read(int fd, uint32_t *value)
{
    struct rp1_access_hw_args args = { .addr = FDEBUG_ADDR, .len = 4, .data = value };

    return ioctl(fd, PIO_IOC_READ_HW, &args) < 0 ? -1 : 0;
}

int fdebug_clear(int fd, uint32_t mask)
{
    struct rp1_access_hw_args args = { .addr = FDEBUG_ADDR, .len = 4, .data = &mask };

    return ioctl(fd, PIO_IOC_WRITE_HW, &args) < 0 ? -1 : 0;
}
