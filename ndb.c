/*
 ============================================================================
 Name        : ndb.c
 Author      : cicerali
 Version     :
 Copyright   : 
 Description : Android phone detector
 Build cmd   : gcc -o ndb ndb.c
 ============================================================================
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/usb/ch9.h>

#define LEVEL_DEBUG 1 << 0

#define PRINT(level, type, format, args...) {\
	if( level & g_level ){\
		fprintf(stdout, "[%5s] %8s: %8s(%d) # ", type, __FILE__, __FUNCTION__, __LINE__);\
		fprintf(stdout, format , ##args);\
		fprintf(stdout, "\n");\
	}\
}

#define LOG_DEBUG(format, args...) PRINT(LEVEL_DEBUG, "DEBUG", format, ##args)

#define ADB_CLASS 0xff
#define ADB_SUBCLASS 0x42
#define ADB_PROTOCOL 0x1

#define USB_DEV_PATH "/dev/bus/usb"
typedef struct dirent dirent;

int contains_non_digit(const char* name);
int is_adb_interface(int usb_class, int usb_subclass, int usb_protocol);

#define  unix_read   adb_read
#define  unix_close  adb_close

int unix_open(const char* path, int options, ...);
int adb_read(int fd, void* buf, size_t len);
int adb_close(int fd);

int g_level = 0x00;

int main(int argc, char *argv[])
{
	if (geteuid() != 0)
	{
		fprintf(stderr, "This application must be run as root\n");
		return EPERM;
	}

	if (argc == 2 && strcmp(argv[1], "-v") == 0)
		g_level = 0x01;
	LOG_DEBUG("Welcome my Lords to ISENGARD");
	char bus[16] =
	{ 0 };
	char dev[16] =
	{ 0 };
	DIR *bus_dir = opendir(USB_DEV_PATH);
	if (bus_dir == NULL)
		return errno;

	LOG_DEBUG("bus_dir:%s opened\n", USB_DEV_PATH);
	dirent* de;
	while ((de = readdir(bus_dir)) != NULL)
	{
		if (contains_non_digit(de->d_name))
			continue;
		char *bus_name = (char *) malloc(
				strlen(USB_DEV_PATH) + strlen(de->d_name) + 2);
		sprintf(bus_name, "%s/%s", USB_DEV_PATH, de->d_name);
		strncpy(bus, de->d_name, 16);

		DIR *dev_dir = opendir(bus_name);
		if (dev_dir == NULL)
			continue;

		LOG_DEBUG("dev_dir:%s opened\n", bus_name);
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
			sprintf(dev_name, "%s/%s", bus_name, de->d_name);
			strncpy(dev, de->d_name, 16);
			LOG_DEBUG("dev_name:%s ", dev_name);

			int fd = unix_open(dev_name, O_RDONLY);
			if (fd == -1)
				continue;
			LOG_DEBUG("opened\n");

			size_t desclength = unix_read(fd, devdesc, sizeof(devdesc));
			bufend = bufptr + desclength;
			if (desclength < USB_DT_DEVICE_SIZE + USB_DT_CONFIG_SIZE)
			{
				LOG_DEBUG("desclength %zu is too small\n", desclength);
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
			LOG_DEBUG("[ %s is V:%04x P:%04x ]\n", dev_name, vid, pid);

			config = (struct usb_config_descriptor *) bufptr;
			bufptr += USB_DT_CONFIG_SIZE;
			if (config->bLength != USB_DT_CONFIG_SIZE
					|| config->bDescriptorType != USB_DT_CONFIG)
			{
				LOG_DEBUG("usb_config_descriptor not found\n");
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
						LOG_DEBUG("interface descriptor has wrong size\n");
						break;
					}
					LOG_DEBUG("bInterfaceClass: %d,  bInterfaceSubClass: %d,"
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
						LOG_DEBUG("looking for bulk endpoints\n");

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
							LOG_DEBUG("endpoints not found\n");
							break;
						}

						if (ep1->bmAttributes != USB_ENDPOINT_XFER_BULK
								|| ep2->bmAttributes != USB_ENDPOINT_XFER_BULK)
						{
							LOG_DEBUG("bulk endpoints not found\n");
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

						LOG_DEBUG("dev_name: %s, devpath: %s\n", dev_name,
								devpath);
						char *serial_path = (char *) malloc(256);
						snprintf(serial_path, 256,
								"/sys/bus/usb/devices/%s/serial", devpath + 4);

						FILE *fp;
						char serial[256] =
						{ 0 };
						fp = fopen(serial_path, "r");
						fgets(serial, 256, fp);
						serial[strcspn(serial, "\n")] = '\0';
						fclose(fp);
						fprintf(stdout, "%s:%s:%s\n", serial, bus, dev);

						break;
					}
				}
				else if(type == USB_DT_CONFIG)
                {
                    LOG_DEBUG("Another configuration found, skipping device\n");
                    break;
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

	LOG_DEBUG("Game over!\n");
	return EXIT_SUCCESS;
}

int contains_non_digit(const char* name)
{
	while (*name)
	{
		if (!isdigit(*name++))
			return 1;
	}
	return 0;
}

int is_adb_interface(int usb_class, int usb_subclass, int usb_protocol)
{
	return (usb_class == ADB_CLASS && usb_subclass == ADB_SUBCLASS
			&& usb_protocol == ADB_PROTOCOL);
}

#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp)            \
  ({                                       \
    long int _rc;                     		\
    do {                                   \
      _rc = (long int) (exp);                         \
    } while (_rc == -1 && errno == EINTR); \
    _rc;                                   \
  })
#endif

int unix_open(const char* path, int options, ...)
{
	if ((options & O_CREAT) == 0)
	{
		return TEMP_FAILURE_RETRY(open(path, options));
	}
	else
	{
		int mode;
		va_list args;
		va_start(args, options);
		mode = va_arg(args, int);
		va_end(args);
		return TEMP_FAILURE_RETRY(open(path, options, mode));
	}
}

int adb_read(int fd, void* buf, size_t len)
{
	return TEMP_FAILURE_RETRY(read(fd, buf, len));
}

int adb_close(int fd)
{
	return close(fd);
}
