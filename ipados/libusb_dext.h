// Drop-in subset of libusb's API for iPadOS, backed by the USBDriverKit dext (see WebUSBDextInterface.h).
// Covers exactly what native/USBBackend.m calls, so that file needs only an #import change.
// DRAFT: never compiled. Not thread-safe; USBBackend serializes all calls on one queue.
#pragma once
#include <stdint.h>
#include <sys/types.h>

enum { LIBUSB_ERROR_IO = -1, LIBUSB_ERROR_INVALID_PARAM = -2, LIBUSB_ERROR_ACCESS = -3, LIBUSB_ERROR_NO_DEVICE = -4,
       LIBUSB_ERROR_NOT_FOUND = -5, LIBUSB_ERROR_BUSY = -6, LIBUSB_ERROR_TIMEOUT = -7, LIBUSB_ERROR_OVERFLOW = -8,
       LIBUSB_ERROR_PIPE = -9, LIBUSB_ERROR_INTERRUPTED = -10, LIBUSB_ERROR_NO_MEM = -11, LIBUSB_ERROR_NOT_SUPPORTED = -12,
       LIBUSB_ERROR_OTHER = -99 };
enum { LIBUSB_ENDPOINT_IN = 0x80, LIBUSB_ENDPOINT_OUT = 0x00 };
enum { LIBUSB_REQUEST_TYPE_STANDARD = 0 << 5, LIBUSB_REQUEST_TYPE_CLASS = 1 << 5, LIBUSB_REQUEST_TYPE_VENDOR = 2 << 5 };
enum { LIBUSB_RECIPIENT_DEVICE = 0, LIBUSB_RECIPIENT_INTERFACE = 1, LIBUSB_RECIPIENT_ENDPOINT = 2, LIBUSB_RECIPIENT_OTHER = 3 };
enum { LIBUSB_REQUEST_SET_ADDRESS = 5, LIBUSB_REQUEST_SET_DESCRIPTOR = 7, LIBUSB_REQUEST_SET_CONFIGURATION = 9, LIBUSB_REQUEST_SET_INTERFACE = 11 };
enum { LIBUSB_TRANSFER_TYPE_MASK = 3, LIBUSB_TRANSFER_TYPE_ISOCHRONOUS = 1, LIBUSB_TRANSFER_TYPE_BULK = 2, LIBUSB_TRANSFER_TYPE_INTERRUPT = 3 };

struct libusb_device_descriptor {
    uint8_t bLength, bDescriptorType; uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};
struct libusb_endpoint_descriptor {
    uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes; uint16_t wMaxPacketSize; uint8_t bInterval;
};
struct libusb_interface_descriptor {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    uint8_t bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
    const struct libusb_endpoint_descriptor *endpoint;
};
struct libusb_interface { const struct libusb_interface_descriptor *altsetting; int num_altsetting; };
struct libusb_config_descriptor {
    uint8_t bLength, bDescriptorType; uint16_t wTotalLength;
    uint8_t bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, MaxPower;
    const struct libusb_interface *interface;
};

typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;

int libusb_init(libusb_context **ctx);
void libusb_exit(libusb_context *ctx);
const char *libusb_error_name(int code);

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list);
void libusb_free_device_list(libusb_device **list, int unref);
libusb_device *libusb_ref_device(libusb_device *dev);
void libusb_unref_device(libusb_device *dev);
/// Stable per physical attachment (IORegistry entry ID); USBBackend uses it as the permission attachment identity.
unsigned long libusb_get_session_data(libusb_device *dev);

int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc);
int libusb_get_config_descriptor(libusb_device *dev, uint8_t index, struct libusb_config_descriptor **config);
int libusb_get_config_descriptor_by_value(libusb_device *dev, uint8_t value, struct libusb_config_descriptor **config);
void libusb_free_config_descriptor(struct libusb_config_descriptor *config);

int libusb_open(libusb_device *dev, libusb_device_handle **handle);
void libusb_close(libusb_device_handle *handle);
int libusb_get_string_descriptor_ascii(libusb_device_handle *handle, uint8_t index, unsigned char *data, int length);
int libusb_get_configuration(libusb_device_handle *handle, int *config);
int libusb_set_configuration(libusb_device_handle *handle, int config);
int libusb_claim_interface(libusb_device_handle *handle, int interface);
int libusb_release_interface(libusb_device_handle *handle, int interface);
int libusb_set_interface_alt_setting(libusb_device_handle *handle, int interface, int alt);
int libusb_clear_halt(libusb_device_handle *handle, unsigned char endpoint);
int libusb_reset_device(libusb_device_handle *handle);
int libusb_control_transfer(libusb_device_handle *handle, uint8_t requestType, uint8_t request, uint16_t value,
                            uint16_t index, unsigned char *data, uint16_t length, unsigned timeout);
int libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint, unsigned char *data, int length,
                         int *transferred, unsigned timeout);
int libusb_interrupt_transfer(libusb_device_handle *handle, unsigned char endpoint, unsigned char *data, int length,
                              int *transferred, unsigned timeout);
