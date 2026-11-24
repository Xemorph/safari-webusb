// libusb subset over the WebUSB dext user client. DRAFT: never compiled or run.
// ASSUMPTIONS TO VERIFY on the Mac: IOKit user-client API availability in the iPadOS SDK, how the dext class is matched
// in the registry, and the async completion callback shape.
#include "libusb_dext.h"
#include "WebUSBDextInterface.h"
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>

struct libusb_context { int unused; };

struct libusb_device {
    int refs;
    uint64_t regid;
    io_service_t service;
    struct libusb_device_descriptor desc;
    uint8_t *cfg[8];          // raw config descriptors, cached at discovery
    uint16_t cfgLength[8];
    struct libusb_device *next;
    int seen;
};

struct libusb_device_handle {
    libusb_device *dev;
    io_connect_t conn;
    mach_vm_address_t buffer;
    mach_vm_size_t size;
    IONotificationPortRef port;
    dispatch_queue_t queue;
    dispatch_semaphore_t slotSem; // counts free transfer slots in the shared buffer
    uint32_t usedSlots;           // bitmask, atomics only
};

static struct libusb_device *g_devices; // single-queue access only

const char *libusb_error_name(int code) {
    switch (code) {
        case LIBUSB_ERROR_IO: return "LIBUSB_ERROR_IO";
        case LIBUSB_ERROR_INVALID_PARAM: return "LIBUSB_ERROR_INVALID_PARAM";
        case LIBUSB_ERROR_ACCESS: return "LIBUSB_ERROR_ACCESS";
        case LIBUSB_ERROR_NO_DEVICE: return "LIBUSB_ERROR_NO_DEVICE";
        case LIBUSB_ERROR_NOT_FOUND: return "LIBUSB_ERROR_NOT_FOUND";
        case LIBUSB_ERROR_BUSY: return "LIBUSB_ERROR_BUSY";
        case LIBUSB_ERROR_TIMEOUT: return "LIBUSB_ERROR_TIMEOUT";
        case LIBUSB_ERROR_OVERFLOW: return "LIBUSB_ERROR_OVERFLOW";
        case LIBUSB_ERROR_PIPE: return "LIBUSB_ERROR_PIPE";
        case LIBUSB_ERROR_INTERRUPTED: return "LIBUSB_ERROR_INTERRUPTED";
        case LIBUSB_ERROR_NO_MEM: return "LIBUSB_ERROR_NO_MEM";
        case LIBUSB_ERROR_NOT_SUPPORTED: return "LIBUSB_ERROR_NOT_SUPPORTED";
        default: return "LIBUSB_ERROR_OTHER";
    }
}

static int fromStatus(uint64_t status) {
    switch (status) {
        case kWebUSBStatusOK: return 0;
        case kWebUSBStatusStall: return LIBUSB_ERROR_PIPE;
        case kWebUSBStatusTimeout: return LIBUSB_ERROR_TIMEOUT;
        case kWebUSBStatusNoDevice: return LIBUSB_ERROR_NO_DEVICE;
        case kWebUSBStatusBusy: return LIBUSB_ERROR_BUSY;
        case kWebUSBStatusAborted: return LIBUSB_ERROR_INTERRUPTED;
        case kWebUSBStatusInvalid: return LIBUSB_ERROR_INVALID_PARAM;
        default: return LIBUSB_ERROR_IO;
    }
}

static int fromIOReturn(kern_return_t kr) {
    if (kr == KERN_SUCCESS) return 0;
    if (kr == kIOReturnNotAttached || kr == kIOReturnNoDevice) return LIBUSB_ERROR_NO_DEVICE;
    if (kr == kIOReturnExclusiveAccess || kr == kIOReturnBusy) return LIBUSB_ERROR_BUSY;
    if (kr == kIOReturnNotPermitted || kr == kIOReturnNotPrivileged) return LIBUSB_ERROR_ACCESS;
    return LIBUSB_ERROR_IO;
}

/* ---- transfer slots: USBBackend overlaps transfers on different endpoints, so each needs its own buffer range ---- */
static int acquireSlot(libusb_device_handle *h) {
    dispatch_semaphore_wait(h->slotSem, DISPATCH_TIME_FOREVER);
    for (;;) {
        uint32_t used = __atomic_load_n(&h->usedSlots, __ATOMIC_ACQUIRE);
        int slot = __builtin_ctz(~used); // the semaphore guarantees a free bit below WEBUSB_SLOTS
        if (__atomic_compare_exchange_n(&h->usedSlots, &used, used | (1u << slot), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return slot;
    }
}
static void releaseSlot(libusb_device_handle *h, int slot) {
    __atomic_fetch_and(&h->usedSlots, ~(1u << slot), __ATOMIC_ACQ_REL);
    dispatch_semaphore_signal(h->slotSem);
}
static uint8_t *slotBase(libusb_device_handle *h, int slot) { return (uint8_t *)h->buffer + (size_t)slot * WEBUSB_MAX_TRANSFER; }

/* ---- simple scalar call helper: returns libusb error, or 0 with out[] filled ---- */
static int call(libusb_device_handle *h, uint32_t selector, const uint64_t *in, uint32_t inCount, uint64_t *out, uint32_t outCount) {
    if (!h) return LIBUSB_ERROR_INVALID_PARAM;
    uint32_t n = outCount;
    return fromIOReturn(IOConnectCallScalarMethod(h->conn, selector, in, inCount, out, &n));
}

/* ---- connection ---- */
static int openConnection(libusb_device *dev, libusb_device_handle **out) {
    libusb_device_handle *h = calloc(1, sizeof *h);
    if (!h) return LIBUSB_ERROR_NO_MEM;
    kern_return_t kr = IOServiceOpen(dev->service, mach_task_self(), 0, &h->conn);
    if (kr != KERN_SUCCESS) { free(h); return fromIOReturn(kr); }
    h->size = WEBUSB_BUFFER_SIZE;
    kr = IOConnectMapMemory64(h->conn, WEBUSB_MEMORY_TRANSFER, mach_task_self(), &h->buffer, &h->size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS) { IOServiceClose(h->conn); free(h); return fromIOReturn(kr); }
    h->slotSem = dispatch_semaphore_create(WEBUSB_SLOTS);
    h->port = IONotificationPortCreate(kIOMainPortDefault);
    h->queue = dispatch_queue_create("webusb.dext.completions", DISPATCH_QUEUE_SERIAL);
    IONotificationPortSetDispatchQueue(h->port, h->queue);
    h->dev = libusb_ref_device(dev);
    *out = h;
    return 0;
}

int libusb_open(libusb_device *dev, libusb_device_handle **handle) { return dev && handle ? openConnection(dev, handle) : LIBUSB_ERROR_INVALID_PARAM; }

void libusb_close(libusb_device_handle *h) {
    if (!h) return;
    IOConnectUnmapMemory64(h->conn, WEBUSB_MEMORY_TRANSFER, mach_task_self(), h->buffer);
    IOServiceClose(h->conn); // dext aborts pipes and releases interfaces on client close
    IONotificationPortDestroy(h->port);
    dispatch_release(h->queue);
    dispatch_release(h->slotSem);
    libusb_unref_device(h->dev);
    free(h);
}

/* ---- descriptors ---- */
static int fetchDescriptor(libusb_device_handle *h, uint8_t type, uint8_t index, uint16_t lang, uint8_t *data, size_t *length) {
    uint64_t in[3] = { type, index, lang };
    return fromIOReturn(IOConnectCallMethod(h->conn, kWebUSBGetDescriptor, in, 3, NULL, 0, NULL, NULL, data, length));
}

/* ---- device cache: one libusb_device per registry entry, so pointer identity matches libusb's behaviour ---- */
static void freeDevice(libusb_device *d) {
    for (int i = 0; i < 8; i++) free(d->cfg[i]);
    IOObjectRelease(d->service);
    free(d);
}

libusb_device *libusb_ref_device(libusb_device *d) { d->refs++; return d; }
void libusb_unref_device(libusb_device *d) { if (d && --d->refs == 0) freeDevice(d); }
unsigned long libusb_get_session_data(libusb_device *d) { return (unsigned long)d->regid; }

static libusb_device *discover(io_service_t service, uint64_t regid) {
    libusb_device *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->refs = 1; d->regid = regid; d->service = service; IOObjectRetain(service);
    libusb_device_handle *h = NULL;
    uint8_t raw[WEBUSB_MAX_DESCRIPTOR];
    size_t len = sizeof raw;
    int ok = openConnection(d, &h) == 0 && fetchDescriptor(h, 1, 0, 0, raw, &len) == 0 && len >= 18;
    if (ok) {
        memcpy(&d->desc, raw, 18); // x86/arm little-endian, same wire layout as the struct
        for (uint8_t i = 0; ok && i < d->desc.bNumConfigurations && i < 8; i++) {
            len = sizeof raw;
            ok = fetchDescriptor(h, 2, i, 0, raw, &len) == 0 && len >= 9;
            if (ok) { d->cfg[i] = malloc(len); ok = d->cfg[i] != NULL; if (ok) { memcpy(d->cfg[i], raw, len); d->cfgLength[i] = (uint16_t)len; } }
        }
    }
    if (h) libusb_close(h);
    if (!ok) { freeDevice(d); return NULL; } // e.g. another client owns it; retry on next enumeration
    return d;
}

int libusb_init(libusb_context **ctx) { *ctx = calloc(1, sizeof **ctx); return *ctx ? 0 : LIBUSB_ERROR_NO_MEM; }
void libusb_exit(libusb_context *ctx) { free(ctx); }

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list) {
    (void)ctx;
    io_iterator_t it;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(WEBUSB_DEXT_CLASS), &it);
    if (kr != KERN_SUCCESS) return fromIOReturn(kr);
    for (libusb_device *d = g_devices; d; d = d->next) d->seen = 0;
    size_t count = 0, capacity = 8;
    libusb_device **result = calloc(capacity + 1, sizeof *result);
    io_service_t service;
    while (result && (service = IOIteratorNext(it))) {
        uint64_t regid = 0;
        IORegistryEntryGetRegistryEntryID(service, &regid);
        libusb_device *d = g_devices;
        while (d && d->regid != regid) d = d->next;
        if (!d && (d = discover(service, regid))) { d->next = g_devices; g_devices = d; }
        if (d) {
            d->seen = 1;
            if (count == capacity) { capacity *= 2; result = realloc(result, (capacity + 1) * sizeof *result); }
            if (!result) { IOObjectRelease(service); break; }
            result[count++] = libusb_ref_device(d);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(it);
    if (!result) return LIBUSB_ERROR_NO_MEM;
    result[count] = NULL;
    for (libusb_device **p = &g_devices; *p;) { // drop the cache's reference to unplugged devices
        if (!(*p)->seen) { libusb_device *gone = *p; *p = gone->next; libusb_unref_device(gone); } else p = &(*p)->next;
    }
    *list = result;
    return (ssize_t)count;
}

void libusb_free_device_list(libusb_device **list, int unref) {
    if (!list) return;
    if (unref) for (libusb_device **p = list; *p; p++) libusb_unref_device(*p);
    free(list);
}

int libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *desc) { *desc = d->desc; return 0; }

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int parseConfig(const uint8_t *raw, size_t length, struct libusb_config_descriptor **out) {
    if (length < 9 || raw[0] < 9) return LIBUSB_ERROR_IO;
    int slots = raw[4], nAlts = 0, perSlot[256] = { 0 }, slotOf[256], used = 0;
    memset(slotOf, -1, sizeof slotOf);
    // Pass 1: group alternate settings by interface number, in order of first appearance.
    for (size_t off = raw[0]; off + 2 <= length && raw[off] >= 2 && off + raw[off] <= length; off += raw[off]) {
        if (raw[off + 1] != 4 || raw[off] < 9) continue;
        uint8_t number = raw[off + 2];
        if (slotOf[number] < 0) { if (used == slots) continue; slotOf[number] = used++; }
        perSlot[slotOf[number]]++; nAlts++;
    }
    struct libusb_config_descriptor *c = calloc(1, sizeof *c);
    struct libusb_interface *interfaces = calloc(slots ? slots : 1, sizeof *interfaces);
    struct libusb_interface_descriptor *block = calloc(nAlts ? nAlts : 1, sizeof *block);
    if (!c || !interfaces || !block) { free(c); free(interfaces); free(block); return LIBUSB_ERROR_NO_MEM; }
    c->bLength = raw[0]; c->bDescriptorType = raw[1]; c->wTotalLength = le16(raw + 2); c->bNumInterfaces = raw[4];
    c->bConfigurationValue = raw[5]; c->iConfiguration = raw[6]; c->bmAttributes = raw[7]; c->MaxPower = raw[8];
    c->interface = interfaces;
    int base = 0, filled[256] = { 0 };
    for (int s = 0; s < used; s++) { interfaces[s].altsetting = block + base; base += perSlot[s]; }
    // Pass 2: fill alt settings; endpoint descriptors follow their interface descriptor.
    struct libusb_interface_descriptor *current = NULL;
    int endpointsSeen = 0;
    for (size_t off = raw[0]; off + 2 <= length && raw[off] >= 2 && off + raw[off] <= length; off += raw[off]) {
        const uint8_t *d = raw + off;
        if (d[1] == 4 && d[0] >= 9 && slotOf[d[2]] >= 0) {
            int s = slotOf[d[2]];
            current = (struct libusb_interface_descriptor *)&interfaces[s].altsetting[filled[s]++];
            interfaces[s].num_altsetting = filled[s];
            current->bLength = d[0]; current->bDescriptorType = d[1]; current->bInterfaceNumber = d[2]; current->bAlternateSetting = d[3];
            current->bNumEndpoints = d[4]; current->bInterfaceClass = d[5]; current->bInterfaceSubClass = d[6];
            current->bInterfaceProtocol = d[7]; current->iInterface = d[8];
            current->endpoint = calloc(d[4] ? d[4] : 1, sizeof(struct libusb_endpoint_descriptor));
            endpointsSeen = 0;
        } else if (d[1] == 5 && d[0] >= 7 && current && current->endpoint && endpointsSeen < current->bNumEndpoints) {
            struct libusb_endpoint_descriptor *e = (struct libusb_endpoint_descriptor *)current->endpoint + endpointsSeen++;
            e->bLength = d[0]; e->bDescriptorType = d[1]; e->bEndpointAddress = d[2]; e->bmAttributes = d[3];
            e->wMaxPacketSize = le16(d + 4); e->bInterval = d[6];
        }
    }
    *out = c;
    return 0;
}

void libusb_free_config_descriptor(struct libusb_config_descriptor *c) {
    if (!c) return;
    for (int i = 0; i < c->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &c->interface[i];
        for (int a = 0; a < itf->num_altsetting; a++) free((void *)itf->altsetting[a].endpoint);
    }
    free((void *)c->interface[0].altsetting); // one block holds every alt setting; slot 0 starts it (NULL if none)
    free((void *)c->interface);
    free(c);
}

int libusb_get_config_descriptor(libusb_device *d, uint8_t index, struct libusb_config_descriptor **config) {
    if (index >= d->desc.bNumConfigurations || index >= 8 || !d->cfg[index]) return LIBUSB_ERROR_NOT_FOUND;
    return parseConfig(d->cfg[index], d->cfgLength[index], config);
}

int libusb_get_config_descriptor_by_value(libusb_device *d, uint8_t value, struct libusb_config_descriptor **config) {
    for (uint8_t i = 0; i < d->desc.bNumConfigurations && i < 8; i++)
        if (d->cfg[i] && d->cfg[i][5] == value) return parseConfig(d->cfg[i], d->cfgLength[i], config);
    return LIBUSB_ERROR_NOT_FOUND;
}

int libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t index, unsigned char *data, int length) {
    if (!h) return LIBUSB_ERROR_INVALID_PARAM;
    if (!index || length < 1) return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t raw[255];
    size_t len = sizeof raw;
    int r = fetchDescriptor(h, 3, index, 0x0409, raw, &len);
    if (r < 0) return r;
    if (len < 2 || raw[0] > len) return LIBUSB_ERROR_IO;
    int n = 0;
    for (size_t i = 2; i + 1 < raw[0] && n < length - 1; i += 2) data[n++] = raw[i + 1] ? '?' : raw[i];
    data[n] = 0;
    return n;
}

/* ---- state operations ---- */
int libusb_get_configuration(libusb_device_handle *h, int *config) {
    uint64_t out[5] = { 0 };
    int r = call(h, kWebUSBGetInfo, NULL, 0, out, 5);
    if (r == 0) *config = (int)out[3];
    return r;
}
int libusb_set_configuration(libusb_device_handle *h, int config) { uint64_t in[1] = { (uint64_t)config }; return call(h, kWebUSBSetConfiguration, in, 1, NULL, 0); }
int libusb_claim_interface(libusb_device_handle *h, int n) { uint64_t in[1] = { (uint64_t)n }; return call(h, kWebUSBClaimInterface, in, 1, NULL, 0); }
int libusb_release_interface(libusb_device_handle *h, int n) { uint64_t in[1] = { (uint64_t)n }; return call(h, kWebUSBReleaseInterface, in, 1, NULL, 0); }
int libusb_set_interface_alt_setting(libusb_device_handle *h, int n, int alt) { uint64_t in[2] = { (uint64_t)n, (uint64_t)alt }; return call(h, kWebUSBSetAltSetting, in, 2, NULL, 0); }
int libusb_clear_halt(libusb_device_handle *h, unsigned char ep) { uint64_t in[1] = { ep }; return call(h, kWebUSBClearHalt, in, 1, NULL, 0); }
int libusb_reset_device(libusb_device_handle *h) { return call(h, kWebUSBReset, NULL, 0, NULL, 0); }

/* ---- transfers: all use buffer offset 0 because USBBackend runs one transfer at a time ---- */
int libusb_control_transfer(libusb_device_handle *h, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                            unsigned char *data, uint16_t length, unsigned timeout) {
    if (!h || length > WEBUSB_MAX_TRANSFER) return LIBUSB_ERROR_INVALID_PARAM;
    int input = type & LIBUSB_ENDPOINT_IN;
    int slot = acquireSlot(h);
    if (!input && length) memcpy(slotBase(h, slot), data, length);
    uint64_t in[7] = { type, request, value, index, length, timeout, (uint64_t)slot * WEBUSB_MAX_TRANSFER }, out[2] = { 0 };
    int r = call(h, kWebUSBControlTransfer, in, 7, out, 2);
    int status = r < 0 ? r : fromStatus(out[0]);
    int result = status < 0 ? status : out[1] > length ? LIBUSB_ERROR_OVERFLOW : (int)out[1];
    if (result > 0 && input) memcpy(data, slotBase(h, slot), (size_t)result);
    releaseSlot(h, slot);
    return result;
}

struct completion { dispatch_semaphore_t done; uint64_t status, actual; IOReturn result; int abandoned; libusb_device_handle *handle; int slot; };

static void completed(void *refcon, IOReturn result, void **args, uint32_t count) {
    struct completion *c = refcon;
    c->result = result;
    c->status = count > 0 ? (uint64_t)(uintptr_t)args[0] : kWebUSBStatusIO;
    c->actual = count > 1 ? (uint64_t)(uintptr_t)args[1] : 0;
    if (__atomic_exchange_n(&c->abandoned, 2, __ATOMIC_ACQ_REL) == 1) { releaseSlot(c->handle, c->slot); dispatch_release(c->done); free(c); }
    else dispatch_semaphore_signal(c->done);
}

static int transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int length, int *transferred, unsigned timeout) {
    if (!h || length < 0 || (unsigned)length > WEBUSB_MAX_TRANSFER) return LIBUSB_ERROR_INVALID_PARAM;
    int input = ep & LIBUSB_ENDPOINT_IN;
    int slot = acquireSlot(h);
    if (!input && length) memcpy(slotBase(h, slot), data, length);
    struct completion *c = calloc(1, sizeof *c);
    if (!c) { releaseSlot(h, slot); return LIBUSB_ERROR_NO_MEM; }
    c->handle = h; c->slot = slot;
    c->done = dispatch_semaphore_create(0);
    io_user_reference_t ref[8] = { 0 };
    ref[kIOAsyncCalloutFuncIndex] = (io_user_reference_t)(uintptr_t)completed;
    ref[kIOAsyncCalloutRefconIndex] = (io_user_reference_t)(uintptr_t)c;
    uint64_t in[4] = { ep, (uint64_t)length, timeout, (uint64_t)slot * WEBUSB_MAX_TRANSFER };
    kern_return_t kr = IOConnectCallAsyncScalarMethod(h->conn, kWebUSBTransfer, IONotificationPortGetMachPort(h->port), ref, kIOAsyncCalloutCount, in, 4, NULL, NULL);
    if (kr != KERN_SUCCESS) { dispatch_release(c->done); free(c); releaseSlot(h, slot); return fromIOReturn(kr); }
    // The dext enforces `timeout`; this guard only catches a dext that never completes.
    int late = dispatch_semaphore_wait(c->done, dispatch_time(DISPATCH_TIME_NOW, ((int64_t)timeout + 2000) * NSEC_PER_MSEC));
    if (late) {
        uint64_t abortIn[1] = { ep };
        call(h, kWebUSBAbortPipe, abortIn, 1, NULL, 0);
        late = dispatch_semaphore_wait(c->done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    }
    if (late) { // the dext may still write into the slot, so the callback frees slot and state if it ever arrives
        if (__atomic_exchange_n(&c->abandoned, 1, __ATOMIC_ACQ_REL) == 2) { releaseSlot(h, slot); dispatch_release(c->done); free(c); }
        return LIBUSB_ERROR_TIMEOUT;
    }
    int status = c->result ? fromIOReturn(c->result) : fromStatus(c->status);
    uint64_t actual = c->actual;
    dispatch_release(c->done); free(c);
    if (actual > (uint64_t)length) { releaseSlot(h, slot); return LIBUSB_ERROR_OVERFLOW; }
    if (input && actual) memcpy(data, slotBase(h, slot), actual);
    releaseSlot(h, slot);
    *transferred = (int)actual;
    return status;
}

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int length, int *transferred, unsigned timeout) {
    return transfer(h, ep, data, length, transferred, timeout);
}
int libusb_interrupt_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int length, int *transferred, unsigned timeout) {
    return transfer(h, ep, data, length, transferred, timeout);
}
