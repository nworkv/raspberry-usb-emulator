// usb_emulator.c
// USB device emulator for Linux USB Raw Gadget.
//
// Usage:
//   sudo ./usb_emulator \
//       --def output/hid_keyboard/device.def \
//       --driver dwc2 \
//       --udc 20980000.usb \
//       --speed full
//
// Optional HID demo:
//   --loop-report 0000040000000000 --interval-ms 1000

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/usb/ch9.h>
#include <linux/usb/raw_gadget.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#define MAX_FILES 32
#define MAX_ENDPOINTS 16
#define MAX_STRING 16
#define MAX_CONFIG 16
#define MAX_REPORT 1024

struct binary_file {
    uint8_t data[MAX_REPORT];
    size_t length;
};

struct endpoint_def {
    uint8_t number;
    uint8_t direction_in;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
    int handle;
};

static struct binary_file g_device;
static struct binary_file g_qualifier;
static struct binary_file g_other_config;
static struct binary_file g_bos;
static struct binary_file g_strings[MAX_STRING];
static struct binary_file g_configs[MAX_CONFIG];
static struct binary_file g_hid_report;

static struct endpoint_def g_endpoints[MAX_ENDPOINTS];
static size_t g_endpoint_count;

static volatile sig_atomic_t g_running = 1;

static void fail(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "error: ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(1);
}

static void on_signal(int signo)
{
    (void)signo;
    g_running = 0;
}

static void load_file(const char *path, struct binary_file *file)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        fail("cannot open %s: %s", path, strerror(errno));

    file->length = fread(file->data, 1, sizeof(file->data), fp);

    if (ferror(fp))
        fail("cannot read %s: %s", path, strerror(errno));

    fclose(fp);

    if (file->length < 2)
        fail("descriptor file %s is too short", path);

    if (file->data[0] != file->length)
        fail("descriptor length mismatch in %s: bLength=%u, file size=%zu",
             path, file->data[0], file->length);
}

static int ioctl_raw(int fd, unsigned long request, void *argument)
{
    int result = ioctl(fd, request, argument);
    if (result < 0)
        fail("ioctl 0x%lx failed: %s", request, strerror(errno));
    return result;
}

static void parse_def(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[1024];

    if (!fp)
        fail("cannot open definition file %s: %s", path, strerror(errno));

    while (fgets(line, sizeof(line), fp)) {
        char key[64];
        char value[512];

        if (line[0] == '#' || line[0] == '\n')
            continue;

        if (sscanf(line, "%63s %511s", key, value) != 2)
            continue;

        if (!strcmp(key, "device")) {
            load_file(value, &g_device);
        } else if (!strcmp(key, "qualifier")) {
            load_file(value, &g_qualifier);
        } else if (!strcmp(key, "other_config")) {
            load_file(value, &g_other_config);
        } else if (!strcmp(key, "bos")) {
            load_file(value, &g_bos);
        } else if (!strncmp(key, "string", 6)) {
            int index = atoi(key + 6);
            if (index < 0 || index >= MAX_STRING)
                fail("invalid string index: %d", index);
            load_file(value, &g_strings[index]);
        } else if (!strncmp(key, "config", 6)) {
            int index = atoi(key + 6);
            if (index < 0 || index >= MAX_CONFIG)
                fail("invalid configuration index: %d", index);
            load_file(value, &g_configs[index]);
        } else if (!strcmp(key, "hid_report")) {
            load_file(value, &g_hid_report);
        } else if (!strcmp(key, "endpoint")) {
            struct endpoint_def *endpoint;
            char direction[16];

            if (g_endpoint_count >= MAX_ENDPOINTS)
                fail("too many endpoints");

            endpoint = &g_endpoints[g_endpoint_count++];

            if (sscanf(value, "%hhu %15s %hhu %hu %hhu",
                       &endpoint->number,
                       direction,
                       &endpoint->attributes,
                       &endpoint->max_packet_size,
                       &endpoint->interval) != 5)
                fail("invalid endpoint line: %s", line);

            endpoint->direction_in = !strcmp(direction, "in");
            endpoint->handle = -1;
        } else {
            fail("unknown definition key: %s", key);
        }
    }

    fclose(fp);

    if (!g_device.length)
        fail("device descriptor is missing");
    if (!g_configs[0].length)
        fail("configuration descriptor 0 is missing");
}

static void ep0_write(int fd, const void *data, size_t length)
{
    struct usb_raw_ep_io io;

    memset(&io, 0, sizeof(io));
    io.ep = 0;
    io.flags = 0;
    io.length = length;

    if (length)
        memcpy(io.data, data, length);

    ioctl_raw(fd, USB_RAW_IOCTL_EP0_WRITE, &io);
}

static void ep0_stall(int fd)
{
    ioctl_raw(fd, USB_RAW_IOCTL_EP0_STALL, NULL);
}

static const struct binary_file *get_descriptor(
    uint8_t type,
    uint8_t index)
{
    switch (type) {
    case USB_DT_DEVICE:
        return &g_device;
    case USB_DT_CONFIGURATION:
        if (index >= MAX_CONFIG || !g_configs[index].length)
            return NULL;
        return &g_configs[index];
    case USB_DT_STRING:
        if (index >= MAX_STRING || !g_strings[index].length)
            return NULL;
        return &g_strings[index];
    case USB_DT_DEVICE_QUALIFIER:
        return g_qualifier.length ? &g_qualifier : NULL;
    case USB_DT_OTHER_SPEED_CONFIGURATION:
        return g_other_config.length ? &g_other_config : NULL;
    case USB_DT_BOS:
        return g_bos.length ? &g_bos : NULL;
    case HID_DT_REPORT:
        return g_hid_report.length ? &g_hid_report : NULL;
    default:
        return NULL;
    }
}

static void handle_control(int fd, struct usb_raw_event *event)
{
    struct usb_ctrlrequest *setup =
        (struct usb_ctrlrequest *)event->data;

    uint8_t request = setup->bRequest;
    uint8_t request_type = setup->bRequestType;
    uint16_t value = le16toh(setup->wValue);
    uint16_t length = le16toh(setup->wLength);

    uint8_t descriptor_type = value >> 8;
    uint8_t descriptor_index = value & 0xFF;

    if ((request_type & USB_TYPE_MASK) != USB_TYPE_STANDARD) {
        if (request_type == 0x81 && request == HID_GET_REPORT &&
            g_hid_report.length) {
            size_t send = length < g_hid_report.length
                ? length
                : g_hid_report.length;
            ep0_write(fd, g_hid_report.data, send);
            return;
        }

        ep0_stall(fd);
        return;
    }

    switch (request) {
    case USB_REQ_GET_DESCRIPTOR: {
        const struct binary_file *descriptor =
            get_descriptor(descriptor_type, descriptor_index);

        if (!descriptor) {
            ep0_stall(fd);
            return;
        }

        size_t send = length < descriptor->length
            ? length
            : descriptor->length;

        ep0_write(fd, descriptor->data, send);
        return;
    }

    case USB_REQ_SET_ADDRESS:
        ep0_write(fd, NULL, 0);
        return;

    case USB_REQ_SET_CONFIGURATION:
        ep0_write(fd, NULL, 0);
        ioctl_raw(fd, USB_RAW_IOCTL_CONFIGURE, NULL);

        for (size_t i = 0; i < g_endpoint_count; i++) {
            struct usb_endpoint_descriptor descriptor;

            memset(&descriptor, 0, sizeof(descriptor));
            descriptor.bLength = USB_DT_ENDPOINT_SIZE;
            descriptor.bDescriptorType = USB_DT_ENDPOINT;
            descriptor.bEndpointAddress =
                g_endpoints[i].number |
                (g_endpoints[i].direction_in ? USB_DIR_IN : USB_DIR_OUT);
            descriptor.bmAttributes = g_endpoints[i].attributes;
            descriptor.wMaxPacketSize =
                htole16(g_endpoints[i].max_packet_size);
            descriptor.bInterval = g_endpoints[i].interval;

            g_endpoints[i].handle = ioctl_raw(
                fd,
                USB_RAW_IOCTL_EP_ENABLE,
                &descriptor);

            printf("enabled EP0x%02X, handle=%d\n",
                   descriptor.bEndpointAddress,
                   g_endpoints[i].handle);
        }
        return;

    case USB_REQ_GET_STATUS: {
        uint16_t status = 0;
        ep0_write(fd, &status, sizeof(status));
        return;
    }

    case USB_REQ_CLEAR_FEATURE:
    case USB_REQ_SET_FEATURE:
        ep0_write(fd, NULL, 0);
        return;

    default:
        ep0_stall(fd);
        return;
    }
}

static void parse_hex_report(
    const char *text,
    uint8_t *report,
    size_t *length)
{
    size_t text_length = strlen(text);

    if (text_length % 2)
        fail("loop report must contain an even number of hex digits");

    *length = text_length / 2;

    if (*length > MAX_REPORT)
        fail("loop report is too long");

    for (size_t i = 0; i < *length; i++) {
        char byte_text[3] = {
            text[i * 2],
            text[i * 2 + 1],
            0
        };

        report[i] = (uint8_t)strtoul(byte_text, NULL, 16);
    }
}

int main(int argc, char **argv)
{
    const char *definition_path = NULL;
    const char *driver_name = "dwc2";
    const char *udc_name = "20980000.usb";
    const char *speed_name = "full";
    const char *loop_report_text = NULL;
    unsigned int loop_interval_ms = 1000;

    uint8_t loop_report[MAX_REPORT];
    size_t loop_report_length = 0;

    int fd;
    int speed;
    struct usb_raw_init init;
    struct usb_raw_event event;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--def") && i + 1 < argc) {
            definition_path = argv[++i];
        } else if (!strcmp(argv[i], "--driver") && i + 1 < argc) {
            driver_name = argv[++i];
        } else if (!strcmp(argv[i], "--udc") && i + 1 < argc) {
            udc_name = argv[++i];
        } else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
            speed_name = argv[++i];
        } else if (!strcmp(argv[i], "--loop-report") && i + 1 < argc) {
            loop_report_text = argv[++i];
        } else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc) {
            loop_interval_ms = (unsigned int)atoi(argv[++i]);
        } else {
            fail("unknown argument: %s", argv[i]);
        }
    }

    if (!definition_path)
        fail("usage: usb_emulator --def device.def [--driver dwc2] "
             "[--udc 20980000.usb] [--speed full] "
             "[--loop-report HEX] [--interval-ms N]");

    if (!strcmp(speed_name, "low"))
        speed = USB_SPEED_LOW;
    else if (!strcmp(speed_name, "full"))
        speed = USB_SPEED_FULL;
    else if (!strcmp(speed_name, "high"))
        speed = USB_SPEED_HIGH;
    else if (!strcmp(speed_name, "super"))
        speed = USB_SPEED_SUPER;
    else
        fail("unknown speed: %s", speed_name);

    parse_def(definition_path);

    if (loop_report_text)
        parse_hex_report(loop_report_text, loop_report, &loop_report_length);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fd = open("/dev/raw-gadget", O_RDWR);
    if (fd < 0)
        fail("cannot open /dev/raw-gadget: %s\n"
             "Ensure that the raw_gadget kernel module is loaded.",
             strerror(errno));

    memset(&init, 0, sizeof(init));

    if (strlen(driver_name) >= UDC_NAME_LENGTH_MAX)
        fail("driver name is too long");

    if (strlen(udc_name) >= UDC_NAME_LENGTH_MAX)
        fail("UDC name is too long");

    strcpy(init.driver_name, driver_name);
    strcpy(init.device_name, udc_name);
    init.speed = speed;

    ioctl_raw(fd, USB_RAW_IOCTL_INIT, &init);
    ioctl_raw(fd, USB_RAW_IOCTL_RUN, NULL);

    printf("USB emulator started\n");
    printf("Driver: %s\n", driver_name);
    printf("UDC: %s\n", udc_name);
    printf("Speed: %s\n", speed_name);
    printf("Press Ctrl+C to stop\n\n");

    while (g_running) {
        memset(&event, 0, sizeof(event));
        ioctl_raw(fd, USB_RAW_IOCTL_EVENT_FETCH, &event);

        switch (event.type) {
        case USB_RAW_EVENT_CONNECT:
            printf("host connected\n");
            break;

        case USB_RAW_EVENT_CONTROL:
            printf("control request: "
                   "type=0x%02X request=0x%02X "
                   "value=0x%04X index=0x%04X length=%u\n",
                   event.data[0],
                   event.data[1],
                   event.data[2] | (event.data[3] << 8),
                   event.data[4] | (event.data[5] << 8),
                   event.data[6] | (event.data[7] << 8));

            handle_control(fd, &event);
            break;

        case USB_RAW_EVENT_SUSPEND:
            printf("bus suspended\n");
            break;

        case USB_RAW_EVENT_RESUME:
            printf("bus resumed\n");
            break;

        case USB_RAW_EVENT_RESET:
            printf("bus reset\n");
            break;

        case USB_RAW_EVENT_DISCONNECT:
            printf("host disconnected\n");
            break;

        default:
            printf("unhandled event type: %u\n", event.type);
            break;
        }

        if (loop_report_length && g_endpoint_count) {
            struct usb_raw_ep_io io;

            memset(&io, 0, sizeof(io));
            io.ep = g_endpoints[0].handle;
            io.flags = 0;
            io.length = loop_report_length;
            memcpy(io.data, loop_report, loop_report_length);

            ioctl_raw(fd, USB_RAW_IOCTL_EP_WRITE, &io);

            usleep(loop_interval_ms * 1000);
        }
    }

    printf("\nstopping\n");

    for (size_t i = 0; i < g_endpoint_count; i++) {
        if (g_endpoints[i].handle >= 0)
            ioctl(fd, USB_RAW_IOCTL_EP_DISABLE, &g_endpoints[i].handle);
    }

    close(fd);
    return 0;
}