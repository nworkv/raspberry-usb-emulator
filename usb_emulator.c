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

#ifndef USB_RAW_DATA_BUFFER_SIZE
#define USB_RAW_DATA_BUFFER_SIZE 4096
#endif
#ifndef USB_RAW_EVENT_BUFFER_SIZE
#define USB_RAW_EVENT_BUFFER_SIZE 4096
#endif
#ifndef HID_DT_REPORT
#define HID_DT_REPORT 0x22
#endif
#ifndef HID_GET_REPORT
#define HID_GET_REPORT 0x01
#endif

#define MAX_ENDPOINTS 16
#define MAX_STRING 16
#define MAX_CONFIG 16
#define MAX_REPORT 1024

struct binary_file { uint8_t data[MAX_REPORT]; size_t length; };
struct endpoint_def { uint8_t number, direction_in, attributes, interval; uint16_t max_packet_size; int handle; };
struct raw_ep_io_buffer { struct usb_raw_ep_io header; uint8_t data[USB_RAW_DATA_BUFFER_SIZE]; };
struct raw_event_buffer { struct usb_raw_event header; uint8_t data[USB_RAW_EVENT_BUFFER_SIZE]; };

static struct binary_file g_device, g_qualifier, g_other_config, g_bos, g_hid_report;
static struct binary_file g_strings[MAX_STRING], g_configs[MAX_CONFIG];
static struct endpoint_def g_endpoints[MAX_ENDPOINTS];
static size_t g_endpoint_count;
static volatile sig_atomic_t g_running = 1;

static void fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "error: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap); exit(1);
}
static void on_signal(int s) { (void)s; g_running = 0; }
static uint16_t read_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void load_file(const char *path, struct binary_file *f) {
    FILE *fp = fopen(path, "rb");
    if (!fp) fail("cannot open %s: %s", path, strerror(errno));
    f->length = fread(f->data, 1, sizeof(f->data), fp);
    if (ferror(fp)) fail("cannot read %s", path);
    fclose(fp);
    if (f->length < 2) fail("descriptor file %s too short", path);
    uint8_t bLength = f->data[0], type = f->data[1];
    if (bLength < 2 || bLength > f->length)
        fail("invalid bLength in %s: %u, size=%zu", path, bLength, f->length);
    switch (type) {
        case USB_DT_DEVICE:
        case USB_DT_DEVICE_QUALIFIER:
        case USB_DT_STRING:
            if (bLength != f->length)
                fail("length mismatch in %s: bLength=%u size=%zu", path, bLength, f->length);
            break;
        case USB_DT_CONFIG:
        case USB_DT_OTHER_SPEED_CONFIG:
        case USB_DT_BOS: {
            if (f->length < 4) fail("composite descriptor %s too short", path);
            uint16_t total = read_le16(&f->data[2]);
            if (total != f->length)
                fail("wTotalLength mismatch in %s: %u vs %zu", path, total, f->length);
            break;
        }
        case HID_DT_REPORT:
            break;
        default:
            break;
    }
}

static int ioctl_raw(int fd, unsigned long req, void *arg) {
    int r = ioctl(fd, req, arg);
    if (r < 0) fail("ioctl 0x%lx failed: %s", req, strerror(errno));
    return r;
}

static void parse_def(const char *path) {
    FILE *fp = fopen(path, "r");
    char line[1024];
    if (!fp) fail("cannot open %s: %s", path, strerror(errno));
    while (fgets(line, sizeof(line), fp)) {
        char key[64], value[512];
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%63s %511s", key, value) != 2) continue;
        if (!strcmp(key, "device")) load_file(value, &g_device);
        else if (!strcmp(key, "qualifier")) load_file(value, &g_qualifier);
        else if (!strcmp(key, "other_config")) load_file(value, &g_other_config);
        else if (!strcmp(key, "bos")) load_file(value, &g_bos);
        else if (!strncmp(key, "string", 6)) { int i = atoi(key+6); if (i >= 0 && i < MAX_STRING) load_file(value, &g_strings[i]); }
        else if (!strncmp(key, "config", 6)) { int i = atoi(key+6); if (i >= 0 && i < MAX_CONFIG) load_file(value, &g_configs[i]); }
        else if (!strcmp(key, "hid_report")) load_file(value, &g_hid_report);
        else if (!strcmp(key, "endpoint")) {
            struct endpoint_def *ep = &g_endpoints[g_endpoint_count++];
            char direction[16];
            if (sscanf(value, "%hhu %15s %hhu %hu %hhu", &ep->number, direction, &ep->attributes, &ep->max_packet_size, &ep->interval) != 5)
                fail("invalid endpoint line");
            ep->direction_in = !strcmp(direction, "in");
            ep->handle = -1;
        } else fail("unknown key: %s", key);
    }
    fclose(fp);
    if (!g_device.length || !g_configs[0].length) fail("device/config descriptor missing");
}

static void ep0_write(int fd, const void *data, size_t length) {
    struct raw_ep_io_buffer buffer;
    struct usb_raw_ep_io *io = &buffer.header;
    if (length > USB_RAW_DATA_BUFFER_SIZE) fail("EP0 response too large");
    memset(&buffer, 0, sizeof(buffer));
    io->ep = 0; io->flags = 0; io->length = length;
    if (length) memcpy(io->data, data, length);
    ioctl_raw(fd, USB_RAW_IOCTL_EP0_WRITE, io);
}

static void ep0_stall(int fd) { ioctl_raw(fd, USB_RAW_IOCTL_EP0_STALL, NULL); }

static const struct binary_file *get_descriptor(uint8_t type, uint8_t index) {
    switch (type) {
        case USB_DT_DEVICE: return &g_device;
        case USB_DT_CONFIG: return (index < MAX_CONFIG && g_configs[index].length) ? &g_configs[index] : NULL;
        case USB_DT_STRING: return (index < MAX_STRING && g_strings[index].length) ? &g_strings[index] : NULL;
        case USB_DT_DEVICE_QUALIFIER: return g_qualifier.length ? &g_qualifier : NULL;
        case USB_DT_OTHER_SPEED_CONFIG: return g_other_config.length ? &g_other_config : NULL;
        case USB_DT_BOS: return g_bos.length ? &g_bos : NULL;
        case HID_DT_REPORT: return g_hid_report.length ? &g_hid_report : NULL;
        default: return NULL;
    }
}

static void handle_control(int fd, struct usb_raw_event *event) {
    if (event->length < sizeof(struct usb_ctrlrequest)) fail("control event too short");
    struct usb_ctrlrequest *setup = (struct usb_ctrlrequest *)event->data;
    uint8_t request = setup->bRequest, type = setup->bRequestType;
    uint16_t value = le16toh(setup->wValue), length = le16toh(setup->wLength);
    uint8_t dtype = value >> 8, dindex = value & 0xFF;

    if ((type & USB_TYPE_MASK) != USB_TYPE_STANDARD) {
        if (type == 0x81 && request == HID_GET_REPORT && g_hid_report.length) {
            size_t send = length < g_hid_report.length ? length : g_hid_report.length;
            ep0_write(fd, g_hid_report.data, send); return;
        }
        ep0_stall(fd); return;
    }

    switch (request) {
        case USB_REQ_GET_DESCRIPTOR: {
            const struct binary_file *d = get_descriptor(dtype, dindex);
            if (!d) { ep0_stall(fd); return; }
            ep0_write(fd, d->data, length < d->length ? length : d->length);
            return;
        }
        case USB_REQ_SET_ADDRESS: ep0_write(fd, NULL, 0); return;
        case USB_REQ_SET_CONFIGURATION: {
            ep0_write(fd, NULL, 0);
            ioctl_raw(fd, USB_RAW_IOCTL_CONFIGURE, NULL);
            for (size_t i = 0; i < g_endpoint_count; i++) {
                struct usb_endpoint_descriptor desc;
                memset(&desc, 0, sizeof(desc));
                desc.bLength = USB_DT_ENDPOINT_SIZE;
                desc.bDescriptorType = USB_DT_ENDPOINT;
                desc.bEndpointAddress = g_endpoints[i].number | (g_endpoints[i].direction_in ? USB_DIR_IN : USB_DIR_OUT);
                desc.bmAttributes = g_endpoints[i].attributes;
                desc.wMaxPacketSize = htole16(g_endpoints[i].max_packet_size);
                desc.bInterval = g_endpoints[i].interval;
                g_endpoints[i].handle = ioctl_raw(fd, USB_RAW_IOCTL_EP_ENABLE, &desc);
                printf("enabled EP0x%02X handle=%d\n", desc.bEndpointAddress, g_endpoints[i].handle);
            }
            return;
        }
        case USB_REQ_GET_STATUS: { uint16_t status = 0; ep0_write(fd, &status, sizeof(status)); return; }
        case USB_REQ_CLEAR_FEATURE:
        case USB_REQ_SET_FEATURE: ep0_write(fd, NULL, 0); return;
        default: ep0_stall(fd); return;
    }
}

static void parse_hex(const char *text, uint8_t *report, size_t *length) {
    size_t n = strlen(text);
    if (n % 2) fail("hex report must have even length");
    *length = n / 2;
    if (*length > MAX_REPORT || *length > USB_RAW_DATA_BUFFER_SIZE) fail("report too large");
    for (size_t i = 0; i < *length; i++) {
        char b[3] = {text[i*2], text[i*2+1], 0};
        report[i] = (uint8_t)strtoul(b, NULL, 16);
    }
}

int main(int argc, char **argv) {
    const char *def = NULL, *driver = "dwc2", *udc = "20980000.usb", *speed_name = "full", *loop_text = NULL;
    unsigned interval_ms = 1000;
    uint8_t loop_report[MAX_REPORT]; size_t loop_length = 0;
    int fd, speed;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--def") && i+1 < argc) def = argv[++i];
        else if (!strcmp(argv[i], "--driver") && i+1 < argc) driver = argv[++i];
        else if (!strcmp(argv[i], "--udc") && i+1 < argc) udc = argv[++i];
        else if (!strcmp(argv[i], "--speed") && i+1 < argc) speed_name = argv[++i];
        else if (!strcmp(argv[i], "--loop-report") && i+1 < argc) loop_text = argv[++i];
        else if (!strcmp(argv[i], "--interval-ms") && i+1 < argc) interval_ms = strtoul(argv[++i], NULL, 0);
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 1; }
    }
    if (!def) { fprintf(stderr, "usage: usb_emulator --def device.def\n"); return 1; }

    speed = !strcmp(speed_name, "low") ? USB_SPEED_LOW : !strcmp(speed_name, "full") ? USB_SPEED_FULL :
            !strcmp(speed_name, "high") ? USB_SPEED_HIGH : !strcmp(speed_name, "super") ? USB_SPEED_SUPER : -1;
    if (speed < 0) fail("unknown speed");

    parse_def(def);
    if (loop_text) parse_hex(loop_text, loop_report, &loop_length);

    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);

    fd = open("/dev/raw-gadget", O_RDWR);
    if (fd < 0) fail("cannot open /dev/raw-gadget: %s", strerror(errno));

    struct usb_raw_init init;
    memset(&init, 0, sizeof(init));
    strcpy((char *)init.driver_name, driver);
    strcpy((char *)init.device_name, udc);
    init.speed = speed;

    ioctl_raw(fd, USB_RAW_IOCTL_INIT, &init);
    ioctl_raw(fd, USB_RAW_IOCTL_RUN, NULL);

    printf("USB emulator started: driver=%s udc=%s speed=%s\n", driver, udc, speed_name);

    struct raw_event_buffer event_buffer;
    struct usb_raw_event *event = &event_buffer.header;

    while (g_running) {
        memset(&event_buffer, 0, sizeof(event_buffer));
        ioctl_raw(fd, USB_RAW_IOCTL_EVENT_FETCH, event);

        switch (event->type) {
            case USB_RAW_EVENT_CONNECT: printf("host connected\n"); break;
            case USB_RAW_EVENT_CONTROL:
                if (event->length >= 8)
                    printf("control: type=0x%02X request=0x%02X value=0x%04X length=%u\n",
                        event->data[0], event->data[1],
                        read_le16(&event->data[2]), read_le16(&event->data[6]));
                handle_control(fd, event);
                break;
            case USB_RAW_EVENT_SUSPEND: printf("suspended\n"); break;
            case USB_RAW_EVENT_RESUME: printf("resumed\n"); break;
            case USB_RAW_EVENT_RESET: printf("reset\n"); break;
            case USB_RAW_EVENT_DISCONNECT: printf("disconnected\n"); break;
            default: break;
        }

        if (loop_length && g_endpoint_count) {
            struct raw_ep_io_buffer io_buffer;
            struct usb_raw_ep_io *io = &io_buffer.header;
            memset(&io_buffer, 0, sizeof(io_buffer));
            io->ep = g_endpoints[0].handle;
            io->flags = 0;
            io->length = loop_length;
            memcpy(io->data, loop_report, loop_length);
            ioctl_raw(fd, USB_RAW_IOCTL_EP_WRITE, io);
            usleep(interval_ms * 1000);
        }
    }

    for (size_t i = 0; i < g_endpoint_count; i++)
        if (g_endpoints[i].handle >= 0)
            ioctl(fd, USB_RAW_IOCTL_EP_DISABLE, &g_endpoints[i].handle);

    close(fd);
    return 0;
}
