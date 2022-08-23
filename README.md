# Bonbon Computer

Bonbon is a compact FPGA computer designed by Lone Dynamics Corporation.

![Bonbon Computer](https://github.com/machdyne/bonbon/blob/9f1226d44e0631d2cd9ea7e3ce0623f97f2f18ad/bonbon.png)

This repo contains schematics, pinouts, example firmware, gateware and documentation.

Find more information on the [Bonbon product page](https://machdyne.com/product/bonbon-computer/).

## Blinky 

Building the blinky example requires [Yosys](https://github.com/YosysHQ/yosys), [nextpnr-ice40](https://github.com/YosysHQ/nextpnr) and [IceStorm](https://github.com/YosysHQ/icestorm).

Assuming they are installed, you can simply type `make` to build the gateware, which will be written to output/blinky.bin. You can then connect the Micro-USB port to your computer and use the latest version of [ldprog](https://github.com/machdyne/ldprog) to write the gateware to the device.

## Programming

The RP2040 firmware, FPGA SRAM and flash MMOD can be programmed over the Micro-USB port.

Configure the FPGA SRAM:

```
$ ldprog -b -s blinky.bin
```

Program the flash MMOD:

```
$ ldprog -b -f blinky.bin
```

Note: It should also be possible to program the MMOD over the USB-C port with a USB bootloader but this is not yet officially supported.

## Firmware

Bonbon ships with RP2040 [firmware](https://github.com/machdyne/bonbon/firmware) based on the [https://github.com/machdyne/musli](Müsli) firmware which allows it to communicate with [ldprog](https://github.com/machdyne/ldprog).

The firmware is responsible for initializing the system, [configuring and outputting the system clock](https://raspberrypi.github.io/pico-sdk-doxygen/group__hardware__clocks.html#details), and either configuring the FPGA or telling the FPGA to configure itself from the MMOD.

The system clock (CLK\_RP) is 126MHz by default.

The firmware can be updated by holding down the BOOTSEL button, connecting the Micro-USB port to your computer, and then dragging and dropping a new UF2 file to the device filesystem.

## Pinouts

 * [MMOD](https://github.com/machdyne/mmod)
 * [DDMI](https://github.com/machdyne/ddmi)
