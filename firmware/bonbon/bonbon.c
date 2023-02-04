/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2022 Lone Dynamics Corporation <info@lonedynamics.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Bonbon Firmware
 *
 */

#define MUSLI_SPI_RX_PIN	28
#define MUSLI_SPI_CSN_PIN	29
#define MUSLI_SPI_SCK_PIN	26
#define MUSLI_SPI_TX_PIN	27

#define MUSLI_CMD_READY 0x00
#define MUSLI_CMD_INIT 0x01

#define MUSLI_CMD_GPIO_SET_DIR 0x10
#define MUSLI_CMD_GPIO_DISABLE_PULLS 0x11
#define MUSLI_CMD_GPIO_PULL_UP 0x12
#define MUSLI_CMD_GPIO_PULL_DOWN 0x13

#define MUSLI_CMD_GPIO_GET 0x20
#define MUSLI_CMD_GPIO_PUT 0x21

#define MUSLI_CMD_SPI_READ 0x80
#define MUSLI_CMD_SPI_WRITE 0x81
#define MUSLI_CMD_CFG_PIO_SPI 0x8f

#define MUSLI_CMD_REBOOT 0xf0

#define ICE40_CDONE 17
#define ICE40_CRESET 19

#define RAM_SS0 4
#define SD_SS 15

#define SPI_MODE_HW 1
#define SPI_MODE_PIO 2

#include <stdio.h>
#include <strings.h>

// Pico
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "pio_spi.h"

#include "hardware/pll.h"
#include "hardware/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/clocks.h"

uint8_t spi_mode = SPI_MODE_HW;

void init_ldprog(void);
void init_gpio(void);
void init_pio_spi(void);
void init_bonbon(void);
void init_bonbon_postconf(void);
void pio_spi_cfg(uint8_t pin_sck, uint8_t pin_mosi, uint8_t pin_miso);

uint8_t spi_pio_offset;

bool bonbon_fpga_configured = false;

pio_spi_inst_t spi_pio = {
	.pio = pio0,
	.sm = 0
};

// For memcpy
#include <string.h>

// Include descriptor struct definitions
#include "usb_common.h"
// USB register definitions from pico-sdk
#include "hardware/regs/usb.h"
// USB hardware struct definitions from pico-sdk
#include "hardware/structs/usb.h"
// For interrupt enable and numbers
#include "hardware/irq.h"
// For resetting the USB controller
#include "hardware/resets.h"

// Device descriptors
#include "bonbon.h"

#define usb_hw_set hw_set_alias(usb_hw)
#define usb_hw_clear hw_clear_alias(usb_hw)

// Function prototypes for our device specific endpoint handlers defined
// later on
void ep0_in_handler(uint8_t *buf, uint16_t len);
void ep0_out_handler(uint8_t *buf, uint16_t len);
void ep1_out_handler(uint8_t *buf, uint16_t len);
void ep2_in_handler(uint8_t *buf, uint16_t len);

// Global device address
static bool should_set_address = false;
static uint8_t dev_addr = 0;
static volatile bool usb_configured = false;

// Global data buffer for EP0
static uint8_t ep0_buf[64];

// Struct defining the device configuration
static struct usb_device_configuration dev_config = {
        .device_descriptor = &device_descriptor,
        .interface_descriptor = &interface_descriptor,
        .config_descriptor = &config_descriptor,
        .lang_descriptor = lang_descriptor,
        .descriptor_strings = descriptor_strings,
        .endpoints = {
                {
                        .descriptor = &ep0_out,
                        .handler = &ep0_out_handler,
                        .endpoint_control = NULL, // NA for EP0
                        .buffer_control = &usb_dpram->ep_buf_ctrl[0].out,
                        // EP0 in and out share a data buffer
                        .data_buffer = &usb_dpram->ep0_buf_a[0],
                },
                {
                        .descriptor = &ep0_in,
                        .handler = &ep0_in_handler,
                        .endpoint_control = NULL, // NA for EP0,
                        .buffer_control = &usb_dpram->ep_buf_ctrl[0].in,
                        // EP0 in and out share a data buffer
                        .data_buffer = &usb_dpram->ep0_buf_a[0],
                },
                {
                        .descriptor = &ep1_out,
                        .handler = &ep1_out_handler,
                        // EP1 starts at offset 0 for endpoint control
                        .endpoint_control = &usb_dpram->ep_ctrl[0].out,
                        .buffer_control = &usb_dpram->ep_buf_ctrl[1].out,
                        // First free EPX buffer
                        .data_buffer = &usb_dpram->epx_data[0 * 64],
                },
                {
                        .descriptor = &ep2_in,
                        .handler = &ep2_in_handler,
                        .endpoint_control = &usb_dpram->ep_ctrl[1].in,
                        .buffer_control = &usb_dpram->ep_buf_ctrl[2].in,
                        // Second free EPX buffer
                        .data_buffer = &usb_dpram->epx_data[1 * 64],
                }
        }
};

/**
 * @brief Given an endpoint address, return the usb_endpoint_configuration of that endpoint. Returns NULL
 * if an endpoint of that address is not found.
 *
 * @param addr
 * @return struct usb_endpoint_configuration*
 */
struct usb_endpoint_configuration *usb_get_endpoint_configuration(uint8_t addr) {
    struct usb_endpoint_configuration *endpoints = dev_config.endpoints;
    for (int i = 0; i < USB_NUM_ENDPOINTS; i++) {
        if (endpoints[i].descriptor && (endpoints[i].descriptor->bEndpointAddress == addr)) {
            return &endpoints[i];
        }
    }
    return NULL;
}

/**
 * @brief Given a C string, fill the EP0 data buf with a USB string descriptor for that string.
 *
 * @param C string you would like to send to the USB host
 * @return the length of the string descriptor in EP0 buf
 */
uint8_t usb_prepare_string_descriptor(const unsigned char *str) {
    // 2 for bLength + bDescriptorType + strlen * 2 because string is unicode. i.e. other byte will be 0
    uint8_t bLength = 2 + (strlen((const char *)str) * 2);
    static const uint8_t bDescriptorType = 0x03;

    volatile uint8_t *buf = &ep0_buf[0];
    *buf++ = bLength;
    *buf++ = bDescriptorType;

    uint8_t c;

    do {
        c = *str++;
        *buf++ = c;
        *buf++ = 0;
    } while (c != '\0');

    return bLength;
}

/**
 * @brief Take a buffer pointer located in the USB RAM and return as an offset of the RAM.
 *
 * @param buf
 * @return uint32_t
 */
static inline uint32_t usb_buffer_offset(volatile uint8_t *buf) {
    return (uint32_t) buf ^ (uint32_t) usb_dpram;
}

/**
 * @brief Set up the endpoint control register for an endpoint (if applicable. Not valid for EP0).
 *
 * @param ep
 */
void usb_setup_endpoint(const struct usb_endpoint_configuration *ep) {
    printf("Set up endpoint 0x%x with buffer address 0x%p\n", ep->descriptor->bEndpointAddress, ep->data_buffer);

    // EP0 doesn't have one so return if that is the case
    if (!ep->endpoint_control) {
        return;
    }

    // Get the data buffer as an offset of the USB controller's DPRAM
    uint32_t dpram_offset = usb_buffer_offset(ep->data_buffer);
    uint32_t reg = EP_CTRL_ENABLE_BITS
                   | EP_CTRL_INTERRUPT_PER_BUFFER
                   | (ep->descriptor->bmAttributes << EP_CTRL_BUFFER_TYPE_LSB)
                   | dpram_offset;
    *ep->endpoint_control = reg;
}

/**
 * @brief Set up the endpoint control register for each endpoint.
 *
 */
void usb_setup_endpoints() {
    const struct usb_endpoint_configuration *endpoints = dev_config.endpoints;
    for (int i = 0; i < USB_NUM_ENDPOINTS; i++) {
        if (endpoints[i].descriptor && endpoints[i].handler) {
            usb_setup_endpoint(&endpoints[i]);
        }
    }
}

/**
 * @brief Set up the USB controller in device mode, clearing any previous state.
 *
 */
void usb_device_init() {
    // Reset usb controller
    reset_block(RESETS_RESET_USBCTRL_BITS);
    unreset_block_wait(RESETS_RESET_USBCTRL_BITS);

    // Clear any previous state in dpram just in case
    memset(usb_dpram, 0, sizeof(*usb_dpram)); // <1>

    // Enable USB interrupt at processor
    irq_set_enabled(USBCTRL_IRQ, true);

    // Mux the controller to the onboard usb phy
    usb_hw->muxing = USB_USB_MUXING_TO_PHY_BITS | USB_USB_MUXING_SOFTCON_BITS;

    // Force VBUS detect so the device thinks it is plugged into a host
    usb_hw->pwr = USB_USB_PWR_VBUS_DETECT_BITS | USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS;

    // Enable the USB controller in device mode.
    usb_hw->main_ctrl = USB_MAIN_CTRL_CONTROLLER_EN_BITS;

    // Enable an interrupt per EP0 transaction
    usb_hw->sie_ctrl = USB_SIE_CTRL_EP0_INT_1BUF_BITS; // <2>

    // Enable interrupts for when a buffer is done, when the bus is reset,
    // and when a setup packet is received
    usb_hw->inte = USB_INTS_BUFF_STATUS_BITS |
                   USB_INTS_BUS_RESET_BITS |
                   USB_INTS_SETUP_REQ_BITS;

    // Set up endpoints (endpoint control registers)
    // described by device configuration
    usb_setup_endpoints();

    // Present full speed device by enabling pull up on DP
    usb_hw_set->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;
}

/**
 * @brief Given an endpoint configuration, returns true if the endpoint
 * is transmitting data to the host (i.e. is an IN endpoint)
 *
 * @param ep, the endpoint configuration
 * @return true
 * @return false
 */
static inline bool ep_is_tx(struct usb_endpoint_configuration *ep) {
    return ep->descriptor->bEndpointAddress & USB_DIR_IN;
}

/**
 * @brief Starts a transfer on a given endpoint.
 *
 * @param ep, the endpoint configuration.
 * @param buf, the data buffer to send. Only applicable if the endpoint is TX
 * @param len, the length of the data in buf (this example limits max len to one packet - 64 bytes)
 */
void usb_start_transfer(struct usb_endpoint_configuration *ep, uint8_t *buf, uint16_t len) {
    // We are asserting that the length is <= 64 bytes for simplicity of the example.
    // For multi packet transfers see the tinyusb port.
    assert(len <= 64);

//    printf("Start transfer of len %d on ep addr 0x%x\n", len, ep->descriptor->bEndpointAddress);

    // Prepare buffer control register value
    uint32_t val = len | USB_BUF_CTRL_AVAIL;

    if (ep_is_tx(ep)) {
        // Need to copy the data from the user buffer to the usb memory
        memcpy((void *) ep->data_buffer, (void *) buf, len);
        // Mark as full
        val |= USB_BUF_CTRL_FULL;
    }

    // Set pid and flip for next transfer
    val |= ep->next_pid ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID;
    ep->next_pid ^= 1u;

    *ep->buffer_control = val;
}

/**
 * @brief Send device descriptor to host
 *
 */
void usb_handle_device_descriptor(void) {
    const struct usb_device_descriptor *d = dev_config.device_descriptor;
    // EP0 in
    struct usb_endpoint_configuration *ep = usb_get_endpoint_configuration(EP0_IN_ADDR);
    // Always respond with pid 1
    ep->next_pid = 1;
    usb_start_transfer(ep, (uint8_t *) d, sizeof(struct usb_device_descriptor));
}

/**
 * @brief Send the configuration descriptor (and potentially the configuration and endpoint descriptors) to the host.
 *
 * @param pkt, the setup packet received from the host.
 */
void usb_handle_config_descriptor(volatile struct usb_setup_packet *pkt) {
    uint8_t *buf = &ep0_buf[0];

    // First request will want just the config descriptor
    const struct usb_configuration_descriptor *d = dev_config.config_descriptor;
    memcpy((void *) buf, d, sizeof(struct usb_configuration_descriptor));
    buf += sizeof(struct usb_configuration_descriptor);

    // If we more than just the config descriptor copy it all
    if (pkt->wLength >= d->wTotalLength) {
        memcpy((void *) buf, dev_config.interface_descriptor, sizeof(struct usb_interface_descriptor));
        buf += sizeof(struct usb_interface_descriptor);
        const struct usb_endpoint_configuration *ep = dev_config.endpoints;

        // Copy all the endpoint descriptors starting from EP1
        for (uint i = 2; i < USB_NUM_ENDPOINTS; i++) {
            if (ep[i].descriptor) {
                memcpy((void *) buf, ep[i].descriptor, sizeof(struct usb_endpoint_descriptor));
                buf += sizeof(struct usb_endpoint_descriptor);
            }
        }

    }

    // Send data
    // Get len by working out end of buffer subtract start of buffer
    uint32_t len = (uint32_t) buf - (uint32_t) &ep0_buf[0];
    usb_start_transfer(usb_get_endpoint_configuration(EP0_IN_ADDR), &ep0_buf[0], len);
}

/**
 * @brief Handle a BUS RESET from the host by setting the device address back to 0.
 *
 */
void usb_bus_reset(void) {
    // Set address back to 0
    dev_addr = 0;
    should_set_address = false;
    usb_hw->dev_addr_ctrl = 0;
    usb_configured = false;
}

/**
 * @brief Send the requested string descriptor to the host.
 *
 * @param pkt, the setup packet from the host.
 */
void usb_handle_string_descriptor(volatile struct usb_setup_packet *pkt) {
    uint8_t i = pkt->wValue & 0xff;
    uint8_t len = 0;

    if (i == 0) {
        len = 4;
        memcpy(&ep0_buf[0], dev_config.lang_descriptor, len);
    } else {
        // Prepare fills in ep0_buf
        len = usb_prepare_string_descriptor(dev_config.descriptor_strings[i - 1]);
    }

    usb_start_transfer(usb_get_endpoint_configuration(EP0_IN_ADDR), &ep0_buf[0], len);
}

/**
 * @brief Sends a zero length status packet back to the host.
 */
void usb_acknowledge_out_request(void) {
    usb_start_transfer(usb_get_endpoint_configuration(EP0_IN_ADDR), NULL, 0);
}

/**
 * @brief Handles a SET_ADDR request from the host. The actual setting of the device address in
 * hardware is done in ep0_in_handler. This is because we have to acknowledge the request first
 * as a device with address zero.
 *
 * @param pkt, the setup packet from the host.
 */
void usb_set_device_address(volatile struct usb_setup_packet *pkt) {
    // Set address is a bit of a strange case because we have to send a 0 length status packet first with
    // address 0
    dev_addr = (pkt->wValue & 0xff);
    printf("Set address %d\r\n", dev_addr);
    // Will set address in the callback phase
    should_set_address = true;
    usb_acknowledge_out_request();
}

/**
 * @brief Handles a SET_CONFIGRUATION request from the host. Assumes one configuration so simply
 * sends a zero length status packet back to the host.
 *
 * @param pkt, the setup packet from the host.
 */
void usb_set_device_configuration(volatile struct usb_setup_packet *pkt) {
    // Only one configuration so just acknowledge the request
    printf("Device Enumerated\r\n");
    usb_acknowledge_out_request();
    usb_configured = true;
}

/**
 * @brief Respond to a setup packet from the host.
 *
 */
void usb_handle_setup_packet(void) {
    volatile struct usb_setup_packet *pkt = (volatile struct usb_setup_packet *) &usb_dpram->setup_packet;
    uint8_t req_direction = pkt->bmRequestType;
    uint8_t req = pkt->bRequest;

    // Reset PID to 1 for EP0 IN
    usb_get_endpoint_configuration(EP0_IN_ADDR)->next_pid = 1u;

    if (req_direction == USB_DIR_OUT) {
        if (req == USB_REQUEST_SET_ADDRESS) {
            usb_set_device_address(pkt);
        } else if (req == USB_REQUEST_SET_CONFIGURATION) {
            usb_set_device_configuration(pkt);
        } else {
            usb_acknowledge_out_request();
            printf("Other OUT request (0x%x)\r\n", pkt->bRequest);
        }
    } else if (req_direction == USB_DIR_IN) {
        if (req == USB_REQUEST_GET_DESCRIPTOR) {
            uint16_t descriptor_type = pkt->wValue >> 8;

            switch (descriptor_type) {
                case USB_DT_DEVICE:
                    usb_handle_device_descriptor();
                    printf("GET DEVICE DESCRIPTOR\r\n");
                    break;

                case USB_DT_CONFIG:
                    usb_handle_config_descriptor(pkt);
                    printf("GET CONFIG DESCRIPTOR\r\n");
                    break;

                case USB_DT_STRING:
                    usb_handle_string_descriptor(pkt);
                    printf("GET STRING DESCRIPTOR\r\n");
                    break;

                default:
                    printf("Unhandled GET_DESCRIPTOR type 0x%x\r\n", descriptor_type);
            }
        } else {
            printf("Other IN request (0x%x)\r\n", pkt->bRequest);
        }
    }
}

/**
 * @brief Notify an endpoint that a transfer has completed.
 *
 * @param ep, the endpoint to notify.
 */
static void usb_handle_ep_buff_done(struct usb_endpoint_configuration *ep) {
    uint32_t buffer_control = *ep->buffer_control;
    // Get the transfer length for this endpoint
    uint16_t len = buffer_control & USB_BUF_CTRL_LEN_MASK;

    // Call that endpoints buffer done handler
    ep->handler((uint8_t *) ep->data_buffer, len);
}

/**
 * @brief Find the endpoint configuration for a specified endpoint number and
 * direction and notify it that a transfer has completed.
 *
 * @param ep_num
 * @param in
 */
static void usb_handle_buff_done(uint ep_num, bool in) {
    uint8_t ep_addr = ep_num | (in ? USB_DIR_IN : 0);
//    printf("EP %d (in = %d) done\n", ep_num, in);
    for (uint i = 0; i < USB_NUM_ENDPOINTS; i++) {
        struct usb_endpoint_configuration *ep = &dev_config.endpoints[i];
        if (ep->descriptor && ep->handler) {
            if (ep->descriptor->bEndpointAddress == ep_addr) {
                usb_handle_ep_buff_done(ep);
                return;
            }
        }
    }
}

/**
 * @brief Handle a "buffer status" irq. This means that one or more
 * buffers have been sent / received. Notify each endpoint where this
 * is the case.
 */
static void usb_handle_buff_status() {
    uint32_t buffers = usb_hw->buf_status;
    uint32_t remaining_buffers = buffers;

    uint bit = 1u;
    for (uint i = 0; remaining_buffers && i < USB_NUM_ENDPOINTS * 2; i++) {
        if (remaining_buffers & bit) {
            // clear this in advance
            usb_hw_clear->buf_status = bit;
            // IN transfer for even i, OUT transfer for odd i
            usb_handle_buff_done(i >> 1u, !(i & 1u));
            remaining_buffers &= ~bit;
        }
        bit <<= 1u;
    }
}

/**
 * @brief USB interrupt handler
 *
 */
/// \tag::isr_setup_packet[]
void isr_usbctrl(void) {
    // USB interrupt handler
    uint32_t status = usb_hw->ints;
    uint32_t handled = 0;

    // Setup packet received
    if (status & USB_INTS_SETUP_REQ_BITS) {
        handled |= USB_INTS_SETUP_REQ_BITS;
        usb_hw_clear->sie_status = USB_SIE_STATUS_SETUP_REC_BITS;
        usb_handle_setup_packet();
    }
/// \end::isr_setup_packet[]

    // Buffer status, one or more buffers have completed
    if (status & USB_INTS_BUFF_STATUS_BITS) {
        handled |= USB_INTS_BUFF_STATUS_BITS;
        usb_handle_buff_status();
    }

    // Bus is reset
    if (status & USB_INTS_BUS_RESET_BITS) {
        printf("BUS RESET\n");
        handled |= USB_INTS_BUS_RESET_BITS;
        usb_hw_clear->sie_status = USB_SIE_STATUS_BUS_RESET_BITS;
        usb_bus_reset();
    }

    if (status ^ handled) {
        panic("Unhandled IRQ 0x%x\n", (uint) (status ^ handled));
    }
}

/**
 * @brief EP0 in transfer complete. Either finish the SET_ADDRESS process, or receive a zero
 * length status packet from the host.
 *
 * @param buf the data that was sent
 * @param len the length that was sent
 */
void ep0_in_handler(uint8_t *buf, uint16_t len) {
    if (should_set_address) {
        // Set actual device address in hardware
        usb_hw->dev_addr_ctrl = dev_addr;
        should_set_address = false;
    } else {
        // Receive a zero length status packet from the host on EP0 OUT
        struct usb_endpoint_configuration *ep = usb_get_endpoint_configuration(EP0_OUT_ADDR);
        usb_start_transfer(ep, NULL, 0);
    }
}

void ep0_out_handler(uint8_t *buf, uint16_t len) {
}

// Device specific functions
void ep1_out_handler(uint8_t *buf, uint16_t len) {

	//	printf("RX %d bytes from host\n", len);
	//	printf("[%x %x %x %x]\n", buf[0], buf[1], buf[2], buf[3]);

	if (buf[0] == MUSLI_CMD_INIT) {
		printf("init %d\n", buf[1]);
		if (buf[1] == 0x00) init_ldprog();
		if (buf[1] == 0x01) init_gpio();
		if (buf[1] == 0x02) init_pio_spi();
		if (buf[1] == 0x03) init_bonbon();
	}

	if (buf[0] == MUSLI_CMD_GPIO_SET_DIR) {
		printf("gpio pin %d direction set to: %d\n", buf[1], buf[2]);
		gpio_set_dir(buf[1], buf[2]);
	}

	if (buf[0] == MUSLI_CMD_GPIO_DISABLE_PULLS) {
		printf("disabling pulls for gpio pin %d\n", buf[1]);
		gpio_disable_pulls(buf[1]);
	}

	if (buf[0] == MUSLI_CMD_GPIO_PULL_UP) {
		printf("enabling pull-up for gpio pin %d\n", buf[1]);
		gpio_pull_up(buf[1]);
	}

	if (buf[0] == MUSLI_CMD_GPIO_PULL_DOWN) {
		printf("enabling pull-down for gpio pin %d\n", buf[1]);
		gpio_pull_down(buf[1]);
	}

	if (buf[0] == MUSLI_CMD_GPIO_GET) {
		uint8_t lbuf[64];
		bzero(lbuf, 64);
		uint8_t val = gpio_get(buf[1]);
		int pd = gpio_is_pulled_down(buf[1]);
		int pu = gpio_is_pulled_up(buf[1]);
		printf("reading gpio %d (%d) [pd: %d pu: %d] ...\n", buf[1], val, pd, pu);
		lbuf[0] = val;
		struct usb_endpoint_configuration *ep =
			usb_get_endpoint_configuration(EP2_IN_ADDR);
		usb_start_transfer(ep, lbuf, len);
	}

	if (buf[0] == MUSLI_CMD_GPIO_PUT) {
		int pd = gpio_is_pulled_down(buf[1]);
		int pu = gpio_is_pulled_up(buf[1]);
		printf("writing %d to gpio %d [pd: %d pu: %d] ...\n", buf[2], buf[1],
			pd, pu);
		gpio_put(buf[1], buf[2]);
	}

	if (buf[0] == MUSLI_CMD_SPI_READ) {
		uint8_t lbuf[64];
		bzero(lbuf, 64);
	//	printf("reading %d bytes from spi [mode: %d] ...\n", buf[1], spi_mode);
		if (spi_mode == SPI_MODE_HW)
			spi_read_blocking(spi1, 0, lbuf, buf[1]);
		else if (spi_mode == SPI_MODE_PIO)
			pio_spi_read8_blocking(&spi_pio, lbuf, buf[1]);
		struct usb_endpoint_configuration *ep =
			usb_get_endpoint_configuration(EP2_IN_ADDR);
		usb_start_transfer(ep, lbuf, len);
	}

	if (buf[0] == MUSLI_CMD_SPI_WRITE) {
	//	printf("writing %d bytes to spi [mode: %d] ...\n", buf[1], spi_mode);
		if (spi_mode == SPI_MODE_HW)
			spi_write_blocking(spi1, buf+4, buf[1]);
		else if (spi_mode == SPI_MODE_PIO)
			pio_spi_write8_blocking(&spi_pio, buf+4, buf[1]);
	}

	if (buf[0] == MUSLI_CMD_CFG_PIO_SPI) {
		pio_spi_cfg(buf[1], buf[2], buf[3]);
	}

	if (buf[0] == MUSLI_CMD_REBOOT) {
		printf("rebooting ...\n", buf[1]);
		watchdog_reboot(0, 0, 0);
	}

	// Get ready to rx again from host
	usb_start_transfer(usb_get_endpoint_configuration(EP1_OUT_ADDR), NULL, 64);

}

void ep2_in_handler(uint8_t *buf, uint16_t len) {
	// Get ready to rx again from host
//	usb_start_transfer(usb_get_endpoint_configuration(EP1_OUT_ADDR), NULL, 64);
}

void init_bonbon(void) {

	spi_mode = SPI_MODE_HW;

	// TODO: init USB as serial device here to output debug messages?

	printf("init_bonbon\n");

	// set these to inputs without pullup/pulldowns
	gpio_init(MUSLI_SPI_CSN_PIN);
	gpio_disable_pulls(MUSLI_SPI_CSN_PIN);
	gpio_init(MUSLI_SPI_RX_PIN);
	gpio_disable_pulls(MUSLI_SPI_RX_PIN);
	gpio_init(MUSLI_SPI_TX_PIN);
	gpio_disable_pulls(MUSLI_SPI_TX_PIN);
	gpio_init(MUSLI_SPI_SCK_PIN);
	gpio_disable_pulls(MUSLI_SPI_SCK_PIN);

	gpio_init(RAM_SS0);
	gpio_disable_pulls(RAM_SS0);

	gpio_init(SD_SS);
	gpio_disable_pulls(SD_SS);

	gpio_init(ICE40_CDONE);
	gpio_disable_pulls(ICE40_CDONE);

}

void init_bonbon_postconf(void) {

   bonbon_fpga_configured = true;

   sleep_ms(100);

   printf("fpga configured.\n");

   sleep_ms(100);

   // release CSPI bus
   gpio_init(MUSLI_SPI_CSN_PIN);
   gpio_init(MUSLI_SPI_TX_PIN);
   gpio_init(MUSLI_SPI_RX_PIN);
   gpio_init(MUSLI_SPI_SCK_PIN);
   gpio_disable_pulls(MUSLI_SPI_CSN_PIN);
   gpio_disable_pulls(MUSLI_SPI_TX_PIN);
   gpio_disable_pulls(MUSLI_SPI_RX_PIN);
   gpio_disable_pulls(MUSLI_SPI_SCK_PIN);

}

void ice40_reset(void) {

	// pull CRESET low
	gpio_init(ICE40_CRESET);
	gpio_disable_pulls(ICE40_CRESET);
	gpio_set_dir(ICE40_CRESET, 1);
	gpio_put(ICE40_CRESET, 0);

	sleep_ms(100);

	// release CRESET
	gpio_disable_pulls(ICE40_CRESET);
	gpio_init(ICE40_CRESET);

	// ICE40 should now load from flash and raise CDONE ...

}

void init_ldprog(void) {

	spi_mode = SPI_MODE_HW;

	uart_init(uart0, 115200);

	gpio_set_function(0, GPIO_FUNC_UART);
	gpio_set_function(1, GPIO_FUNC_UART);

	printf("init_ldprog\n");

	gpio_init(MUSLI_SPI_CSN_PIN);
	gpio_disable_pulls(MUSLI_SPI_CSN_PIN);

	gpio_set_function(MUSLI_SPI_RX_PIN, GPIO_FUNC_SPI);
	gpio_set_function(MUSLI_SPI_SCK_PIN, GPIO_FUNC_SPI);
	gpio_set_function(MUSLI_SPI_TX_PIN, GPIO_FUNC_SPI);
	spi_init(spi1, 1000 * 1000);

	gpio_init(ICE40_CDONE);
	gpio_init(ICE40_CRESET);
	gpio_disable_pulls(ICE40_CDONE);
	gpio_disable_pulls(ICE40_CRESET);

}

void init_gpio(void) {
	printf("init_gpio\n");
	spi_deinit(spi1);
	uart_deinit(uart0);
	for (int i = 0; i <= 3; i++) {
		gpio_init(i);
		gpio_disable_pulls(i);
	}
	for (int i = 8; i <= 11; i++) {
		gpio_init(i);
		gpio_disable_pulls(i);
	}
}

void init_pio_spi(void) {

	spi_mode = SPI_MODE_PIO;

	printf("init_pio_spi\n");

	for (int i = 8; i <= 11; i++) {
		gpio_init(i);
		gpio_disable_pulls(i);
	}

}

void pio_spi_cfg(uint8_t pin_sck, uint8_t pin_mosi, uint8_t pin_miso) {

	printf("pio_spi sck %d mosi %d miso %d\n", pin_sck, pin_mosi, pin_miso);

  	pio_spi_init(spi_pio.pio, spi_pio.sm, spi_pio_offset,
		8,       // 8 bits per SPI frame
		31.25f,  // 1 MHz @ 125 clk_sys
		false,   // CPHA = 0
		false,   // CPOL = 0
		pin_sck,
		pin_mosi,
		pin_miso
	);
}

int main(void) {

	// set the sys clock to 126mhz
	set_sys_clock_khz(126000, true);

	stdio_init_all();
	printf("Bonbon initializing ...\n");

/*
	printf("clocks: ");
    uint f_pll_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_SYS_CLKSRC_PRIMARY);
    uint f_pll_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_USB_CLKSRC_PRIMARY);
    uint f_rosc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_ROSC_CLKSRC);
    uint f_clk_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    uint f_clk_peri = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_PERI);
    uint f_clk_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB);
    uint f_clk_adc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_ADC);
    uint f_clk_rtc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_RTC);
 
    printf("pll_sys  = %dkHz\n", f_pll_sys);
    printf("pll_usb  = %dkHz\n", f_pll_usb);
    printf("rosc     = %dkHz\n", f_rosc);
    printf("clk_sys  = %dkHz\n", f_clk_sys);
    printf("clk_peri = %dkHz\n", f_clk_peri);
    printf("clk_usb  = %dkHz\n", f_clk_usb);
    printf("clk_adc  = %dkHz\n", f_clk_adc);
    printf("clk_rtc  = %dkHz\n", f_clk_rtc);
*/

	printf("enable clock output ...\n");
//	clock_gpio_init(24, CLOCKS_CLK_GPOUT2_CTRL_AUXSRC_VALUE_CLK_SYS, 1);
	clock_gpio_init(24, CLOCKS_CLK_GPOUT2_CTRL_AUXSRC_VALUE_CLK_USB, 1);
//	clock_gpio_init(24, CLOCKS_CLK_GPOUT2_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, 1);

	printf("usb_device_init ...\n");
	usb_device_init();

	printf("loading spi_pio ...\n");
	spi_pio_offset = pio_add_program(spi_pio.pio, &spi_cpha0_program);
	printf("loaded PIO program at %d\n", spi_pio_offset);

	init_bonbon();
	ice40_reset();

   int kfc_prev = 0;
   int usb_started = 0;

	// Everything is interrupt driven so just loop here
	while (1) {

		tight_loop_contents();

		if (usb_configured && !usb_started) {
			usb_start_transfer(usb_get_endpoint_configuration(EP1_OUT_ADDR),
				NULL, 64);
			usb_started = 1;
		}

		int cdone = gpio_get(ICE40_CDONE);
		if (cdone && cdone != kfc_prev && !bonbon_fpga_configured) {
			init_bonbon_postconf();
		}
		kfc_prev = cdone;

	}

	return 0;

}
