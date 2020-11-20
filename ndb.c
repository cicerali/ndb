/*
 ============================================================================
 Name        : ndb.c
 Author      : Ali Temel Cicerali
 Version     :
 Copyright   : 
 Description : Android phone detector
 ============================================================================
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <linux/usb/ch9.h>

#include "utils.h"

int main(void)
{

	DIR *bus_dir = opendir(USB_DEV_PATH);
	if (bus_dir == NULL)
		return errno;

	dirent* de;
	while ((de = readdir(bus_dir)) != NULL)
	{
		if (contains_non_digit(de->d_name))
			continue;
		char *bus_name = (char *) malloc(
				strlen(USB_DEV_PATH) + strlen(de->d_name) + 2);
		sprintf(bus_name, "%s/%s", USB_DEV_PATH, de->d_name);

		DIR *dev_dir = opendir(bus_name);
		if (dev_dir == NULL)
			continue;

		while ((de = readdir(dev_dir)))
		{
			unsigned char devdesc[4096];
			unsigned char* bufptr = devdesc;
			unsigned char* bufend;
			struct usb_device_descriptor* device;
			struct usb_config_descriptor* config;
			struct usb_interface_descriptor* interface;
			struct usb_endpoint_descriptor *ep1, *ep2;
			unsigned zero_mask = 0;
			size_t max_packet_size = 0;
			unsigned vid, pid;

			if (contains_non_digit(de->d_name))
				continue;

			char *dev_name = (char *) malloc(
					strlen(bus_name) + strlen(de->d_name) + 2);
			sprintf(bus_name, "%s/%s", bus_name, de->d_name);

			int fd = unix_open(dev_name, O_RDONLY | O_CLOEXEC);
			if (fd == -1)
				continue;

			size_t desclength = unix_read(fd, devdesc, sizeof(devdesc));
			bufend = bufptr + desclength;
			if (desclength < USB_DT_DEVICE_SIZE + USB_DT_CONFIG_SIZE)
			{
				fprintf(stderr, "desclength %zu is too small\n", desclength);
				unix_close(fd);
				continue;
			}

			device = (struct usb_device_descriptor*) bufptr;
			bufptr += USB_DT_DEVICE_SIZE;
			if ((device->bLength != USB_DT_DEVICE_SIZE)
					|| (device->bDescriptorType != USB_DT_DEVICE))
			{
				unix_close(fd);
				continue;
			}
			vid = device->idVendor;
			pid = device->idProduct;
			fprintf(stderr, "[ %s is V:%04x P:%04x ]\n", dev_name, vid, pid);

			config = (struct usb_config_descriptor *) bufptr;
			bufptr += USB_DT_CONFIG_SIZE;
			if (config->bLength != USB_DT_CONFIG_SIZE
					|| config->bDescriptorType != USB_DT_CONFIG)
			{
				fprintf(stderr, "usb_config_descriptor not found\n");
				unix_close(fd);
				continue;
			}

			while (bufptr < bufend)
			{
				unsigned char length = bufptr[0];
				unsigned char type = bufptr[1];
				if (type == USB_DT_INTERFACE)
				{
					interface = (struct usb_interface_descriptor *) bufptr;
					bufptr += length;

					if (length != USB_DT_INTERFACE_SIZE)
					{
						fprintf(stderr, "interface descriptor has wrong size\n");
						break;
					}
					fprintf(stderr, "bInterfaceClass: %d,  bInterfaceSubClass: %d,"
							"bInterfaceProtocol: %d, bNumEndpoints: %d\n",
							interface->bInterfaceClass,
							interface->bInterfaceSubClass,
							interface->bInterfaceProtocol,
							interface->bNumEndpoints);

					if (interface->bNumEndpoints == 2
							&& is_adb_interface(interface->bInterfaceClass,
									interface->bInterfaceSubClass,
									interface->bInterfaceProtocol))
					{
						struct stat st;
						char pathbuf[128];
						char link[256];
						char *devpath = NULL;
						fprintf(stderr, "looking for bulk endpoints\n");

						ep1 = (struct usb_endpoint_descriptor *) bufptr;
						bufptr += USB_DT_ENDPOINT_SIZE;
						if (bufptr + 2 <= devdesc + desclength&&
						bufptr[0] == USB_DT_SS_EP_COMP_SIZE &&
						bufptr[1] == USB_DT_SS_ENDPOINT_COMP)
						{
							bufptr += USB_DT_SS_EP_COMP_SIZE;
						}

						ep2 = (struct usb_endpoint_descriptor *) bufptr;
						bufptr += USB_DT_ENDPOINT_SIZE;
						if (bufptr + 2 <= devdesc + desclength&&
						bufptr[0] == USB_DT_SS_EP_COMP_SIZE &&
						bufptr[1] == USB_DT_SS_ENDPOINT_COMP)
						{
							bufptr += USB_DT_SS_EP_COMP_SIZE;
						}

						if (bufptr > devdesc + desclength||
						ep1->bLength != USB_DT_ENDPOINT_SIZE ||
						ep1->bDescriptorType != USB_DT_ENDPOINT ||
						ep2->bLength != USB_DT_ENDPOINT_SIZE ||
						ep2->bDescriptorType != USB_DT_ENDPOINT)
						{
							fprintf(stderr, "endpoints not found\n");
							break;
						}

						if (ep1->bmAttributes != USB_ENDPOINT_XFER_BULK
								|| ep2->bmAttributes != USB_ENDPOINT_XFER_BULK)
						{
							fprintf(stderr, "bulk endpoints not found\n");
							continue;
						}

						if (interface->bInterfaceProtocol == ADB_PROTOCOL)
						{
							max_packet_size = ep1->wMaxPacketSize;
							zero_mask = ep1->wMaxPacketSize - 1;
						}

						unsigned char local_ep_in, local_ep_out;
						if (ep1->bEndpointAddress & USB_ENDPOINT_DIR_MASK)
						{
							local_ep_in = ep1->bEndpointAddress;
							local_ep_out = ep2->bEndpointAddress;
						}
						else
						{
							local_ep_in = ep2->bEndpointAddress;
							local_ep_out = ep1->bEndpointAddress;
						}

						if (!fstat(fd, &st) && S_ISCHR(st.st_mode))
						{
							snprintf(pathbuf, sizeof(pathbuf),
									"/sys/dev/char/%d:%d", major(st.st_rdev),
									minor(st.st_rdev));
							ssize_t link_len = readlink(pathbuf, link,
									sizeof(link) - 1);
							if (link_len > 0)
							{
								link[link_len] = '\0';
								const char* slash = strrchr(link, '/');
								if (slash)
								{
									snprintf(pathbuf, sizeof(pathbuf), "usb:%s",
											slash + 1);
									devpath = pathbuf;
								}
							}
						}

						fprintf(stderr, "dev_name: %s, devpath: %s\n", dev_name, devpath);
						break;
					}
				}
				else
				{
					bufptr += length;
				}
			}
			unix_close(fd);
			free(dev_name);
		}
		if (dev_dir != NULL)
			closedir(dev_dir);

		free(bus_name);
	}

	if (bus_dir != NULL)
		closedir(bus_dir);

	fprintf(stderr, "End of Game\n");
	return EXIT_SUCCESS;
}
