// Wire interface between the iPadOS companion app and the USBDriverKit dext.
// Shared by both targets (C, no Foundation). DESIGN DRAFT: nothing here is compiled or tested yet.
//
// Mapping to the libusb calls USBBackend.m uses today:
//   libusb_get_device_descriptor / get_config_descriptor / get_string_descriptor -> GetDescriptor (raw bytes, parsed app-side)
//   libusb_get_configuration / set_configuration                                -> GetConfiguration / SetConfiguration
//   libusb_claim_interface / release_interface / set_interface_alt_setting      -> Claim / Release / SetAlt
//   libusb_clear_halt / reset_device                                            -> ClearHalt / Reset
//   libusb_control_transfer                                                     -> ControlTransfer (sync)
//   libusb_bulk_transfer / interrupt_transfer                                   -> Transfer (async) + AbortPipe
//   libusb_open / libusb_close                                                  -> IOServiceOpen / IOServiceClose (one exclusive client)
//   libusb_get_device_list + hotplug                                            -> IOServiceMatching(dext class) + IOServiceAddMatchingNotification
//
// Security boundary is the dext's Info.plist match (idVendor) plus the entitlement, not this header.
// Protected-class filtering stays in USBBackend.m; the dext is a raw pipe.
#pragma once
#include <stdint.h>

#define WEBUSB_DEXT_CLASS            "WebUSBDextDevice"   // IOService match name
#define WEBUSB_MAX_TRANSFER          (1u << 20)           // equals USBMaximumTransfer in USBBackend.m
#define WEBUSB_MAX_DESCRIPTOR        4096u                // structure-output cap for GetDescriptor

// Shared buffer: IOConnectMapMemory64(conn, WEBUSB_MEMORY_TRANSFER, ...). Transfers name a byte range in it.
// The app owns offsets: it splits the buffer into WEBUSB_SLOTS slots of WEBUSB_MAX_TRANSFER bytes and gives each
// in-flight transfer its own slot, so transfers on different endpoints can overlap without sharing bytes.
#define WEBUSB_MEMORY_TRANSFER       0
#define WEBUSB_SLOTS                 8u                   // max concurrent transfers per connection
#define WEBUSB_BUFFER_SIZE           (WEBUSB_SLOTS * WEBUSB_MAX_TRANSFER)

// IOConnectCall* selectors. "in" = scalars the app sends, "out" = scalars returned.
typedef enum {
    // in: none                         out: [vendorId, productId, speed, activeConfig, locationId]
    kWebUSBGetInfo = 0,
    // in: [descType, descIndex, langId] out: structure = raw descriptor bytes (<= WEBUSB_MAX_DESCRIPTOR)
    // Config descriptors are returned whole (wTotalLength), so the app can reuse its libusb-style parsing.
    kWebUSBGetDescriptor = 1,
    // in: [configValue]                out: none. Fails if any interface is claimed.
    kWebUSBSetConfiguration = 2,
    // in: [interfaceNumber]            out: none. Opens the IOUSBHostInterface; never detaches another driver.
    kWebUSBClaimInterface = 3,
    // in: [interfaceNumber]            out: none. Aborts its pipes first.
    kWebUSBReleaseInterface = 4,
    // in: [interfaceNumber, alt]       out: none
    kWebUSBSetAltSetting = 5,
    // in: [endpointAddress]            out: none
    kWebUSBClearHalt = 6,
    // in: none                         out: none. Re-enumerates; the app must re-claim afterwards.
    kWebUSBReset = 7,
    // in: [bmRequestType, bRequest, wValue, wIndex, wLength, timeoutMs, bufferOffset]
    // out: [status, actualLength]. For IN the data lands at bufferOffset; for OUT it is read from there.
    kWebUSBControlTransfer = 8,
    // ASYNC: call with IOConnectCallAsyncScalarMethod and a wake port.
    // in: [endpointAddress, length, timeoutMs, bufferOffset]  out: none
    // Completion message args: [status, actualLength]. Needed because a CAN receive pipe can stay pending for minutes.
    kWebUSBTransfer = 9,
    // in: [endpointAddress]            out: none. Pending transfer completes with kWebUSBStatusAborted.
    kWebUSBAbortPipe = 10,
    kWebUSBSelectorCount
} WebUSBSelector;

// Same meaning as libusb's error codes so USBBackend's existing error mapping keeps working.
typedef enum {
    kWebUSBStatusOK = 0,
    kWebUSBStatusStall = 1,        // libusb_ERROR_PIPE, reported to the page as status "stall"
    kWebUSBStatusTimeout = 2,
    kWebUSBStatusNoDevice = 3,
    kWebUSBStatusBusy = 4,         // interface already claimed / another client connected
    kWebUSBStatusAborted = 5,
    kWebUSBStatusInvalid = 6,      // bad selector argument, offset+length outside the shared buffer
    kWebUSBStatusIO = 7
} WebUSBStatus;

// Dext lifecycle rules (not enforceable in a header):
//  * One user client at a time (matches USBBackend's `owners` check); a second open returns kWebUSBStatusBusy.
//  * Client close, device removal or app termination: abort all pipes, release all interfaces.
//  * Validate bufferOffset + length <= WEBUSB_BUFFER_SIZE on every call; length <= WEBUSB_MAX_TRANSFER.
