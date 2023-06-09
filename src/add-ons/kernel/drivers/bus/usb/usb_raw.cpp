/*
 * Copyright 2006-2018, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 *		Adrien Destugues <pulkomandy@pulkomandy.tk>
 */

#include "usb_raw.h"

#include <util/AutoLock.h>
#include <AutoDeleter.h>
#include <KernelExport.h>
#include <Drivers.h>
#include <algorithm>
#include <lock.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <kernel.h>


//#define TRACE_USB_RAW
#ifdef TRACE_USB_RAW
#define TRACE(x)	dprintf x
#else
#define TRACE(x)	/* nothing */
#endif

#define DRIVER_NAME		"usb_raw"
#define DEVICE_NAME		"bus/usb/raw"

typedef struct {
	usb_device			device;
	mutex				lock;
	uint32				reference_count;

	char				name[64];
	void				*link;

	sem_id				notify;
	status_t			status;
	size_t				actual_length;
} raw_device;

int32 api_version = B_CUR_DRIVER_API_VERSION;
static usb_module_info *gUSBModule = NULL;
static raw_device *gDeviceList = NULL;
static uint32 gDeviceCount = 0;
static mutex gDeviceListLock;
static char **gDeviceNames = NULL;

static status_t
usb_raw_device_added(usb_device newDevice, void **cookie)
{
	TRACE((DRIVER_NAME": device_added(0x%08" B_PRIx32 ")\n", newDevice));
	raw_device *device = (raw_device *)malloc(sizeof(raw_device));

	mutex_init(&device->lock, "usb_raw device lock");

	device->notify = create_sem(0, "usb_raw callback notify");
	if (device->notify < B_OK) {
		mutex_destroy(&device->lock);
		free(device);
		return B_NO_MORE_SEMS;
	}

	char deviceName[64];
	memcpy(deviceName, &newDevice, sizeof(usb_device));
	if (gUSBModule->usb_ioctl('DNAM', deviceName, sizeof(deviceName)) >= B_OK) {
		snprintf(device->name, sizeof(device->name), "bus/usb/%s", deviceName);
	} else {
		snprintf(device->name, sizeof(device->name), "bus/usb/%08" B_PRIx32,
			newDevice);
	}

	device->device = newDevice;
	device->reference_count = 0;

	mutex_lock(&gDeviceListLock);
	device->link = (void *)gDeviceList;
	gDeviceList = device;
	gDeviceCount++;
	mutex_unlock(&gDeviceListLock);

	TRACE((DRIVER_NAME": new device: 0x%p\n", device));
	*cookie = (void *)device;
	return B_OK;
}


static status_t
usb_raw_device_removed(void *cookie)
{
	TRACE((DRIVER_NAME": device_removed(0x%p)\n", cookie));
	raw_device *device = (raw_device *)cookie;

	// cancel all pending transfers to make sure no one keeps waiting forever
	// in syscalls.
	const usb_configuration_info *configurationInfo =
		gUSBModule->get_configuration(device->device);
	if (configurationInfo != NULL) {
		struct usb_interface_info* interface
			= configurationInfo->interface->active;
		for (unsigned int i = 0; i < interface->endpoint_count; i++)
			gUSBModule->cancel_queued_transfers(interface->endpoint[i].handle);
	}

	mutex_lock(&gDeviceListLock);
	if (gDeviceList == device) {
		gDeviceList = (raw_device *)device->link;
	} else {
		raw_device *element = gDeviceList;
		while (element) {
			if (element->link == device) {
				element->link = device->link;
				break;
			}

			element = (raw_device *)element->link;
		}
	}
	gDeviceCount--;
	mutex_unlock(&gDeviceListLock);

	device->device = 0;
	if (device->reference_count == 0) {
		mutex_lock(&device->lock);
		mutex_destroy(&device->lock);
		delete_sem(device->notify);
		free(device);
	}

	return B_OK;
}


//
//#pragma mark -
//


static const usb_configuration_info *
usb_raw_get_configuration(raw_device *device, uint32 configIndex,
	status_t *status)
{
	const usb_configuration_info *result = gUSBModule->get_nth_configuration(
		device->device, configIndex);
	if (result == NULL) {
		*status = B_USB_RAW_STATUS_INVALID_CONFIGURATION;
		return NULL;
	}

	return result;
}


static const usb_interface_info *
usb_raw_get_interface(raw_device *device, uint32 configIndex,
	uint32 interfaceIndex, uint32 alternateIndex, status_t *status)
{
	const usb_configuration_info *configurationInfo
		= usb_raw_get_configuration(device, configIndex, status);
	if (configurationInfo == NULL)
		return NULL;

	if (interfaceIndex >= configurationInfo->interface_count) {
		*status = B_USB_RAW_STATUS_INVALID_INTERFACE;
		return NULL;
	}

	const usb_interface_info *result = NULL;
	if (alternateIndex == B_USB_RAW_ACTIVE_ALTERNATE)
		result = configurationInfo->interface[interfaceIndex].active;
	else {
		const usb_interface_list *interfaceList =
			&configurationInfo->interface[interfaceIndex];
		if (alternateIndex >= interfaceList->alt_count) {
			*status = B_USB_RAW_STATUS_INVALID_INTERFACE;
			return NULL;
		}

		result = &interfaceList->alt[alternateIndex];
	}

	return result;
}


//
//#pragma mark -
//


static status_t
usb_raw_open(const char *name, uint32 flags, void **cookie)
{
	TRACE((DRIVER_NAME": open()\n"));
	mutex_lock(&gDeviceListLock);
	raw_device *element = gDeviceList;
	while (element) {
		if (strcmp(name, element->name) == 0) {
			element->reference_count++;
			*cookie = element;
			mutex_unlock(&gDeviceListLock);
			return B_OK;
		}

		element = (raw_device *)element->link;
	}

	mutex_unlock(&gDeviceListLock);
	return B_NAME_NOT_FOUND;
}


static status_t
usb_raw_close(void *cookie)
{
	TRACE((DRIVER_NAME": close()\n"));
	return B_OK;
}


static status_t
usb_raw_free(void *cookie)
{
	TRACE((DRIVER_NAME": free()\n"));
	mutex_lock(&gDeviceListLock);

	raw_device *device = (raw_device *)cookie;
	device->reference_count--;
	if (device->device == 0) {
		mutex_lock(&device->lock);
		mutex_destroy(&device->lock);
		delete_sem(device->notify);
		free(device);
	}

	mutex_unlock(&gDeviceListLock);
	return B_OK;
}


static void
usb_raw_callback(void *cookie, status_t status, void *data, size_t actualLength)
{
	TRACE((DRIVER_NAME": callback()\n"));
	raw_device *device = (raw_device *)cookie;

	switch (status) {
		case B_OK:
			device->status = B_USB_RAW_STATUS_SUCCESS;
			break;
		case B_TIMED_OUT:
			device->status = B_USB_RAW_STATUS_TIMEOUT;
			break;
		case B_CANCELED:
			device->status = B_USB_RAW_STATUS_ABORTED;
			break;
		case B_DEV_CRC_ERROR:
			device->status = B_USB_RAW_STATUS_CRC_ERROR;
			break;
		case B_DEV_STALLED:
			device->status = B_USB_RAW_STATUS_STALLED;
			break;
		default:
			device->status = B_USB_RAW_STATUS_FAILED;
			break;
	}

	device->actual_length = actualLength;
	release_sem(device->notify);
}


static status_t
usb_raw_ioctl(void *cookie, uint32 op, void *buffer, size_t length)
{
	TRACE((DRIVER_NAME": ioctl\n"));
	raw_device *device = (raw_device *)cookie;
	if (device->device == 0)
		return B_DEV_NOT_READY;

	usb_raw_command command;
	if (length < sizeof(command.version.status))
		return B_BUFFER_OVERFLOW;
	if (!IS_USER_ADDRESS(buffer)
		|| user_memcpy(&command, buffer, min_c(length, sizeof(command)))
			!= B_OK) {
		return B_BAD_ADDRESS;
	}

	command.version.status = B_USB_RAW_STATUS_ABORTED;
	status_t status = B_DEV_INVALID_IOCTL;

	switch (op) {
		case B_USB_RAW_COMMAND_GET_VERSION:
		{
			command.version.status = B_USB_RAW_PROTOCOL_VERSION;
			status = B_OK;
			break;
		}

		case B_USB_RAW_COMMAND_GET_DEVICE_DESCRIPTOR:
		{
			if (length < sizeof(command.device))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_device_descriptor *deviceDescriptor =
				gUSBModule->get_device_descriptor(device->device);
			if (deviceDescriptor == NULL)
				return B_OK;

			if (!IS_USER_ADDRESS(command.device.descriptor)
				|| user_memcpy(command.device.descriptor, deviceDescriptor,
					sizeof(usb_device_descriptor)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			command.device.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_GET_CONFIGURATION_DESCRIPTOR:
		case B_USB_RAW_COMMAND_GET_CONFIGURATION_DESCRIPTOR_ETC:
		{
			if (op == B_USB_RAW_COMMAND_GET_CONFIGURATION_DESCRIPTOR_ETC
					&& length < sizeof(command.config_etc))
				return B_BUFFER_OVERFLOW;

			if (length < sizeof(command.config))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_configuration_info *configurationInfo =
				usb_raw_get_configuration(device, command.config.config_index,
					&command.config.status);
			if (configurationInfo == NULL)
				break;

			size_t sizeToCopy = sizeof(usb_configuration_descriptor);
			if (op == B_USB_RAW_COMMAND_GET_CONFIGURATION_DESCRIPTOR_ETC) {
				sizeToCopy = std::min(command.config_etc.length,
					(size_t)configurationInfo->descr->total_length);
			}

			if (!IS_USER_ADDRESS(command.config.descriptor)
				|| user_memcpy(command.config.descriptor,
					configurationInfo->descr, sizeToCopy) != B_OK) {
				return B_BAD_ADDRESS;
			}

			command.config.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_GET_ALT_INTERFACE_COUNT:
		case B_USB_RAW_COMMAND_GET_ACTIVE_ALT_INTERFACE_INDEX:
		{
			if (length < sizeof(command.alternate))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_configuration_info *configurationInfo =
				usb_raw_get_configuration(device,
					command.alternate.config_index,
					&command.alternate.status);
			if (configurationInfo == NULL)
				break;

			if (command.alternate.interface_index
				>= configurationInfo->interface_count) {
				command.alternate.status = B_USB_RAW_STATUS_INVALID_INTERFACE;
				break;
			}

			const usb_interface_list *interfaceList
				= &configurationInfo->interface[
					command.alternate.interface_index];
			if (op == B_USB_RAW_COMMAND_GET_ALT_INTERFACE_COUNT) {
				command.alternate.alternate_info = interfaceList->alt_count;
			} else {
				for (size_t i = 0; i < interfaceList->alt_count; i++) {
					if (&interfaceList->alt[i] == interfaceList->active) {
						command.alternate.alternate_info = i;
						break;
					}
				}
			}

			command.alternate.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_GET_INTERFACE_DESCRIPTOR:
		case B_USB_RAW_COMMAND_GET_INTERFACE_DESCRIPTOR_ETC:
		{
			const usb_interface_info *interfaceInfo = NULL;
			status = B_OK;
			if (op == B_USB_RAW_COMMAND_GET_INTERFACE_DESCRIPTOR) {
				if (length < sizeof(command.interface))
					return B_BUFFER_OVERFLOW;

				interfaceInfo = usb_raw_get_interface(device,
					command.interface.config_index,
					command.interface.interface_index,
					B_USB_RAW_ACTIVE_ALTERNATE,
					&command.interface.status);
			} else {
				if (length < sizeof(command.interface_etc))
					return B_BUFFER_OVERFLOW;

				interfaceInfo = usb_raw_get_interface(device,
					command.interface_etc.config_index,
					command.interface_etc.interface_index,
					command.interface_etc.alternate_index,
					&command.interface_etc.status);
			}

			if (interfaceInfo == NULL)
				break;

			if (!IS_USER_ADDRESS(command.interface.descriptor)
				|| user_memcpy(command.interface.descriptor, interfaceInfo->descr,
					sizeof(usb_interface_descriptor)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			command.interface.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_GET_ENDPOINT_DESCRIPTOR:
		case B_USB_RAW_COMMAND_GET_ENDPOINT_DESCRIPTOR_ETC:
		{
			uint32 endpointIndex = 0;
			const usb_interface_info *interfaceInfo = NULL;
			status = B_OK;
			if (op == B_USB_RAW_COMMAND_GET_ENDPOINT_DESCRIPTOR) {
				if (length < sizeof(command.endpoint))
					return B_BUFFER_OVERFLOW;

				interfaceInfo = usb_raw_get_interface(device,
					command.endpoint.config_index,
					command.endpoint.interface_index,
					B_USB_RAW_ACTIVE_ALTERNATE,
					&command.endpoint.status);
				endpointIndex = command.endpoint.endpoint_index;
			} else {
				if (length < sizeof(command.endpoint_etc))
					return B_BUFFER_OVERFLOW;

				interfaceInfo = usb_raw_get_interface(device,
					command.endpoint_etc.config_index,
					command.endpoint_etc.interface_index,
					command.endpoint_etc.alternate_index,
					&command.endpoint_etc.status);
				endpointIndex = command.endpoint_etc.endpoint_index;
			}

			if (interfaceInfo == NULL)
				break;

			if (endpointIndex >= interfaceInfo->endpoint_count) {
				command.endpoint.status = B_USB_RAW_STATUS_INVALID_ENDPOINT;
				break;
			}

			if (!IS_USER_ADDRESS(command.endpoint.descriptor)
				|| user_memcpy(command.endpoint.descriptor,
					interfaceInfo->endpoint[endpointIndex].descr,
					sizeof(usb_endpoint_descriptor)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			command.endpoint.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_GET_GENERIC_DESCRIPTOR:
		case B_USB_RAW_COMMAND_GET_GENERIC_DESCRIPTOR_ETC:
		{
			uint32 genericIndex = 0;
			size_t genericLength = 0;
			const usb_interface_info *interfaceInfo = NULL;
			status = B_OK;
			if (op == B_USB_RAW_COMMAND_GET_GENERIC_DESCRIPTOR) {
				if (length < sizeof(command.generic))
					return B_BUFFER_OVERFLOW;

				interfaceInfo = usb_raw_get_interface(device,
					command.generic.config_index,
					command.generic.interface_index,
					B_USB_RAW_ACTIVE_ALTERNATE,
					&command.generic.status);
				genericIndex = command.generic.generic_index;
				genericLength = command.generic.length;
			} else {
				if (length < sizeof(command.generic_etc))
					return B_BUFFER_OVERFLOW;

				interfaceInfo = usb_raw_get_interface(device,
					command.generic_etc.config_index,
					command.generic_etc.interface_index,
					command.generic_etc.alternate_index,
					&command.generic_etc.status);
				genericIndex = command.generic_etc.generic_index;
				genericLength = command.generic_etc.length;
			}

			if (interfaceInfo == NULL)
				break;

			if (genericIndex >= interfaceInfo->generic_count) {
				command.endpoint.status = B_USB_RAW_STATUS_INVALID_ENDPOINT;
				break;
			}

			usb_descriptor *descriptor = interfaceInfo->generic[genericIndex];
			if (descriptor == NULL)
				break;

			if (!IS_USER_ADDRESS(command.generic.descriptor)
				|| user_memcpy(command.generic.descriptor, descriptor,
					min_c(genericLength, descriptor->generic.length)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			if (descriptor->generic.length > genericLength)
				command.generic.status = B_USB_RAW_STATUS_NO_MEMORY;
			else
				command.generic.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_GET_STRING_DESCRIPTOR:
		{
			if (length < sizeof(command.string))
				return B_BUFFER_OVERFLOW;

			size_t actualLength = 0;
			uint8 temp[4];
			status = B_OK;

			// We need to fetch the default language code, first.
			if (gUSBModule->get_descriptor(device->device,
				USB_DESCRIPTOR_STRING, 0, 0,
				temp, 4, &actualLength) < B_OK
				|| actualLength != 4
				|| temp[1] != USB_DESCRIPTOR_STRING) {
				command.string.status = B_USB_RAW_STATUS_ABORTED;
				command.string.length = 0;
				break;
			}
			const uint16 langid = (temp[2] | (temp[3] << 8));

			// Now fetch the string length.
			if (gUSBModule->get_descriptor(device->device,
				USB_DESCRIPTOR_STRING, command.string.string_index, langid,
				temp, 2, &actualLength) < B_OK
				|| actualLength != 2
				|| temp[1] != USB_DESCRIPTOR_STRING) {
				command.string.status = B_USB_RAW_STATUS_ABORTED;
				command.string.length = 0;
				break;
			}

			uint8 stringLength = MIN(temp[0], command.string.length);
			char *string = (char *)malloc(stringLength);
			if (string == NULL) {
				command.string.status = B_USB_RAW_STATUS_ABORTED;
				command.string.length = 0;
				status = B_NO_MEMORY;
				break;
			}

			if (gUSBModule->get_descriptor(device->device,
				USB_DESCRIPTOR_STRING, command.string.string_index, langid,
				string, stringLength, &actualLength) < B_OK
				|| actualLength != stringLength) {
				command.string.status = B_USB_RAW_STATUS_ABORTED;
				command.string.length = 0;
				free(string);
				break;
			}

			if (!IS_USER_ADDRESS(command.string.descriptor)
				|| user_memcpy(command.string.descriptor, string,
					stringLength) != B_OK) {
				free(string);
				return B_BAD_ADDRESS;
			}

			command.string.status = B_USB_RAW_STATUS_SUCCESS;
			command.string.length = stringLength;
			free(string);
			break;
		}

		case B_USB_RAW_COMMAND_GET_DESCRIPTOR:
		{
			if (length < sizeof(command.descriptor))
				return B_BUFFER_OVERFLOW;

			size_t actualLength = 0;
			uint8 firstTwoBytes[2];
			status = B_OK;

			if (gUSBModule->get_descriptor(device->device,
				command.descriptor.type, command.descriptor.index,
				command.descriptor.language_id, firstTwoBytes, 2,
				&actualLength) < B_OK
				|| actualLength != 2
				|| firstTwoBytes[1] != command.descriptor.type) {
				command.descriptor.status = B_USB_RAW_STATUS_ABORTED;
				command.descriptor.length = 0;
				break;
			}

			uint8 descriptorLength = MIN(firstTwoBytes[0],
				command.descriptor.length);
			uint8 *descriptorBuffer = (uint8 *)malloc(descriptorLength);
			if (descriptorBuffer == NULL) {
				command.descriptor.status = B_USB_RAW_STATUS_ABORTED;
				command.descriptor.length = 0;
				status = B_NO_MEMORY;
				break;
			}

			if (gUSBModule->get_descriptor(device->device,
				command.descriptor.type, command.descriptor.index,
				command.descriptor.language_id, descriptorBuffer,
				descriptorLength, &actualLength) < B_OK
				|| actualLength != descriptorLength) {
				command.descriptor.status = B_USB_RAW_STATUS_ABORTED;
				command.descriptor.length = 0;
				free(descriptorBuffer);
				break;
			}

			if (!IS_USER_ADDRESS(command.descriptor.data)
				|| user_memcpy(command.descriptor.data, descriptorBuffer,
					descriptorLength) != B_OK) {
				free(descriptorBuffer);
				return B_BAD_ADDRESS;
			}

			command.descriptor.status = B_USB_RAW_STATUS_SUCCESS;
			command.descriptor.length = descriptorLength;
			free(descriptorBuffer);
			break;
		}

		case B_USB_RAW_COMMAND_SET_CONFIGURATION:
		{
			if (length < sizeof(command.config))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_configuration_info *configurationInfo =
				usb_raw_get_configuration(device, command.config.config_index,
					&command.config.status);
			if (configurationInfo == NULL)
				break;

			if (gUSBModule->set_configuration(device->device,
				configurationInfo) < B_OK) {
				command.config.status = B_USB_RAW_STATUS_FAILED;
				break;
			}

			command.config.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_SET_ALT_INTERFACE:
		{
			if (length < sizeof(command.alternate))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_configuration_info *configurationInfo =
				usb_raw_get_configuration(device,
					command.alternate.config_index,
					&command.alternate.status);
			if (configurationInfo == NULL)
				break;

			if (command.alternate.interface_index
				>= configurationInfo->interface_count) {
				command.alternate.status = B_USB_RAW_STATUS_INVALID_INTERFACE;
				break;
			}

			const usb_interface_list *interfaceList =
				&configurationInfo->interface[command.alternate.interface_index];
			if (command.alternate.alternate_info >= interfaceList->alt_count) {
				command.alternate.status = B_USB_RAW_STATUS_INVALID_INTERFACE;
				break;
			}

			if (gUSBModule->set_alt_interface(device->device,
				&interfaceList->alt[command.alternate.alternate_info]) < B_OK) {
				command.alternate.status = B_USB_RAW_STATUS_FAILED;
				break;
			}

			command.alternate.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_CONTROL_TRANSFER:
		{
			if (length < sizeof(command.control))
				return B_BUFFER_OVERFLOW;

			void *controlData = malloc(command.control.length);
			if (controlData == NULL)
				return B_NO_MEMORY;
			MemoryDeleter dataDeleter(controlData);

			bool inTransfer = (command.control.request_type
				& USB_ENDPOINT_ADDR_DIR_IN) != 0;
			if (!IS_USER_ADDRESS(command.control.data)
				|| (!inTransfer && user_memcpy(controlData,
					command.control.data, command.control.length) != B_OK)) {
				return B_BAD_ADDRESS;
			}

			MutexLocker deviceLocker(device->lock);
			if (gUSBModule->queue_request(device->device,
				command.control.request_type, command.control.request,
				command.control.value, command.control.index,
				command.control.length, controlData,
				usb_raw_callback, device) < B_OK) {
				command.control.status = B_USB_RAW_STATUS_FAILED;
				command.control.length = 0;
				status = B_OK;
				break;
			}

			status = acquire_sem_etc(device->notify, 1, B_KILL_CAN_INTERRUPT, 0);
			if (status != B_OK) {
				gUSBModule->cancel_queued_requests(device->device);
				acquire_sem(device->notify);
			}

			command.control.status = device->status;
			command.control.length = device->actual_length;
			deviceLocker.Unlock();

			if (command.control.status == B_OK)
				status = B_OK;
			if (inTransfer && user_memcpy(command.control.data, controlData,
				command.control.length) != B_OK) {
				status = B_BAD_ADDRESS;
			}
			break;
		}

		case B_USB_RAW_COMMAND_INTERRUPT_TRANSFER:
		case B_USB_RAW_COMMAND_BULK_TRANSFER:
		case B_USB_RAW_COMMAND_ISOCHRONOUS_TRANSFER:
		{
			if (length < sizeof(command.transfer))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_configuration_info *configurationInfo =
				gUSBModule->get_configuration(device->device);
			if (configurationInfo == NULL) {
				command.transfer.status = B_USB_RAW_STATUS_INVALID_CONFIGURATION;
				break;
			}

			if (command.transfer.interface >= configurationInfo->interface_count) {
				command.transfer.status = B_USB_RAW_STATUS_INVALID_INTERFACE;
				break;
			}

			const usb_interface_info *interfaceInfo =
				configurationInfo->interface[command.transfer.interface].active;
			if (interfaceInfo == NULL) {
				command.transfer.status = B_USB_RAW_STATUS_ABORTED;
				break;
			}

			if (command.transfer.endpoint >= interfaceInfo->endpoint_count) {
				command.transfer.status = B_USB_RAW_STATUS_INVALID_ENDPOINT;
				break;
			}

			const usb_endpoint_info *endpointInfo =
				&interfaceInfo->endpoint[command.transfer.endpoint];
			if (!endpointInfo->handle) {
				command.transfer.status = B_USB_RAW_STATUS_INVALID_ENDPOINT;
				break;
			}

			size_t descriptorsSize = 0;
			usb_iso_packet_descriptor *packetDescriptors = NULL;
			void *transferData = NULL;
			MemoryDeleter descriptorsDeleter, dataDeleter;

			bool inTransfer = (endpointInfo->descr->endpoint_address
				& USB_ENDPOINT_ADDR_DIR_IN) != 0;
			if (op == B_USB_RAW_COMMAND_ISOCHRONOUS_TRANSFER) {
				if (length < sizeof(command.isochronous))
					return B_BUFFER_OVERFLOW;

				descriptorsSize = sizeof(usb_iso_packet_descriptor)
					* command.isochronous.packet_count;
				packetDescriptors
					= (usb_iso_packet_descriptor *)malloc(descriptorsSize);
				if (packetDescriptors == NULL) {
					command.transfer.status = B_USB_RAW_STATUS_NO_MEMORY;
					command.transfer.length = 0;
					break;
				}
				descriptorsDeleter.SetTo(packetDescriptors);

				if (!IS_USER_ADDRESS(command.isochronous.data)
					|| !IS_USER_ADDRESS(command.isochronous.packet_descriptors)
					|| user_memcpy(packetDescriptors,
						command.isochronous.packet_descriptors,
						descriptorsSize) != B_OK) {
					return B_BAD_ADDRESS;
				}
			} else {
				transferData = malloc(command.transfer.length);
				if (transferData == NULL) {
					command.transfer.status = B_USB_RAW_STATUS_NO_MEMORY;
					command.transfer.length = 0;
					break;
				}
				dataDeleter.SetTo(transferData);

				if (!IS_USER_ADDRESS(command.transfer.data) || (!inTransfer
						&& user_memcpy(transferData, command.transfer.data,
							command.transfer.length) != B_OK)) {
					return B_BAD_ADDRESS;
				}
			}

			status_t status;
			MutexLocker deviceLocker(device->lock);
			if (op == B_USB_RAW_COMMAND_INTERRUPT_TRANSFER) {
				status = gUSBModule->queue_interrupt(endpointInfo->handle,
					transferData, command.transfer.length,
					usb_raw_callback, device);
			} else if (op == B_USB_RAW_COMMAND_BULK_TRANSFER) {
				status = gUSBModule->queue_bulk(endpointInfo->handle,
					transferData, command.transfer.length,
					usb_raw_callback, device);
			} else {
				status = gUSBModule->queue_isochronous(endpointInfo->handle,
					command.isochronous.data, command.isochronous.length,
					packetDescriptors, command.isochronous.packet_count, NULL,
					0, usb_raw_callback, device);
			}

			if (status < B_OK) {
				command.transfer.status = B_USB_RAW_STATUS_FAILED;
				command.transfer.length = 0;
				status = B_OK;
				break;
			}

			status = acquire_sem_etc(device->notify, 1, B_KILL_CAN_INTERRUPT, 0);
			if (status != B_OK) {
				gUSBModule->cancel_queued_transfers(endpointInfo->handle);
				acquire_sem(device->notify);
			}

			command.transfer.status = device->status;
			command.transfer.length = device->actual_length;
			deviceLocker.Unlock();

			if (command.transfer.status == B_OK)
				status = B_OK;
			if (op == B_USB_RAW_COMMAND_ISOCHRONOUS_TRANSFER) {
				if (user_memcpy(command.isochronous.packet_descriptors,
						packetDescriptors, descriptorsSize) != B_OK) {
					status = B_BAD_ADDRESS;
				}
			} else {
				if (inTransfer && user_memcpy(command.transfer.data,
					transferData, command.transfer.length) != B_OK) {
					status = B_BAD_ADDRESS;
				}
			}

			break;
		}
	}

	if (user_memcpy(buffer, &command, min_c(length, sizeof(command))) != B_OK)
		return B_BAD_ADDRESS;

	return status;
}


static status_t
usb_raw_read(void *cookie, off_t position, void *buffer, size_t *length)
{
	TRACE((DRIVER_NAME": read()\n"));
	*length = 0;
	return B_OK;
}


static status_t
usb_raw_write(void *cookie, off_t position, const void *buffer, size_t *length)
{
	TRACE((DRIVER_NAME": write()\n"));
	*length = 0;
	return B_OK;
}


//
//#pragma mark -
//


status_t
init_hardware()
{
	TRACE((DRIVER_NAME": init_hardware()\n"));
	return B_OK;
}


status_t
init_driver()
{
	TRACE((DRIVER_NAME": init_driver()\n"));
	static usb_notify_hooks notifyHooks = {
		&usb_raw_device_added,
		&usb_raw_device_removed
	};

	gDeviceList = NULL;
	gDeviceCount = 0;
	mutex_init(&gDeviceListLock, "usb_raw device list lock");

	TRACE((DRIVER_NAME": trying module %s\n", B_USB_MODULE_NAME));
	status_t result = get_module(B_USB_MODULE_NAME,
		(module_info **)&gUSBModule);
	if (result < B_OK) {
		TRACE((DRIVER_NAME": getting module failed 0x%08" B_PRIx32 "\n",
			result));
		mutex_destroy(&gDeviceListLock);
		return result;
	}

	gUSBModule->register_driver(DRIVER_NAME, NULL, 0, NULL);
	gUSBModule->install_notify(DRIVER_NAME, &notifyHooks);
	return B_OK;
}


void
uninit_driver()
{
	TRACE((DRIVER_NAME": uninit_driver()\n"));
	gUSBModule->uninstall_notify(DRIVER_NAME);
	mutex_lock(&gDeviceListLock);

	if (gDeviceNames) {
		for (int32 i = 1; gDeviceNames[i]; i++)
			free(gDeviceNames[i]);
		free(gDeviceNames);
		gDeviceNames = NULL;
	}

	mutex_destroy(&gDeviceListLock);
	put_module(B_USB_MODULE_NAME);
}


const char **
publish_devices()
{
	TRACE((DRIVER_NAME": publish_devices()\n"));
	if (gDeviceNames) {
		for (int32 i = 0; gDeviceNames[i]; i++)
			free(gDeviceNames[i]);
		free(gDeviceNames);
		gDeviceNames = NULL;
	}

	int32 index = 0;
	gDeviceNames = (char **)malloc(sizeof(char *) * (gDeviceCount + 2));
	if (gDeviceNames == NULL)
		return NULL;

	gDeviceNames[index++] = strdup(DEVICE_NAME);

	mutex_lock(&gDeviceListLock);
	raw_device *element = gDeviceList;
	while (element) {
		gDeviceNames[index++] = strdup(element->name);
		element = (raw_device *)element->link;
	}

	gDeviceNames[index++] = NULL;
	mutex_unlock(&gDeviceListLock);
	return (const char **)gDeviceNames;
}


device_hooks *
find_device(const char *name)
{
	TRACE((DRIVER_NAME": find_device()\n"));
	static device_hooks hooks = {
		&usb_raw_open,
		&usb_raw_close,
		&usb_raw_free,
		&usb_raw_ioctl,
		&usb_raw_read,
		&usb_raw_write,
		NULL,
		NULL,
		NULL,
		NULL
	};

	return &hooks;
}
