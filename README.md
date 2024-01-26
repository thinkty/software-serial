# Software Serial

A software approach to serial communication using Linux (specifically [Raspbian](https://www.raspberrypi.com/software/) w/ [Device Tree](https://www.raspberrypi.com/documentation/computers/configuration.html#device-trees-overlays-and-parameters)).
The code is based on [n7d-lkm](https://github.com/thinkty/n7d-lkm) which is a 7-segment display device driver for Linux.

## Requirements
- **kernel headers** : the kernel headers are needed to compile a kernel module. The version to download will depend on your (target) kernel version.
- **dtc** : the RPi uses [device tree](https://www.kernel.org/doc/Documentation/devicetree/usage-model.txt) for hardware enumeration. The `dtc` command will be used to compile the [overlay](https://www.raspberrypi.com/documentation/computers/configuration.html#part2) and it should already be installed by default.

## Install
Clone the repository, run `make` to compile the device tree overlay and the kernel module.

### Device Tree Overlay
This kernel module uses the GPIO pins specified in the fragments in the overlay [`overlay.dts`](https://github.com/thinkty/software-serial/blob/main/overlay.dts).
Therefore, the overlay must be compiled, and put into the `/boot/firmware/overlays/` directory for it to be accessible on boot.
To apply the overlay, it must be specified in `/boot/config.txt`.
For example,
```
# Compile the overlay
dtc -@ -I dts -O dtb -o $(DT_OVERLAY).dtbo $(DT_OVERLAY).dts

# Place the overlay in the overlays dir
cp $(DT_OVERLAY).dtbo /boot/firmware/overlays/.

# Edit config.txt to include the overlay (specify parameters if needed)
echo "dtoverlay=$(DT_OVERLAY)" >> /boot/config.txt
```

After rebooting, check that the device tree has been properly parsed by running
```
dtc -I fs /sys/firmware/devicetree/base | less
```

### Kernel Module
To install the kernel module, run :
```
insmod soft-serial.ko
```

`sudo` or some access may be needed due to permission.
`modprobe` may be used instead of `insmod` but there are no other dependencies for this module.
The baudrate (default 38400) for the serial communication can be specified during module installation:
```
insmod soft-serial.ko baudrate=19200
```

To remove (uninstall) the kernel module, run :
```
rmmod soft_serial
```

## License
GPL
