Very simple, work in progress input driver for the SPI keyboard / trackpad found on 12" MacBooks (2015 and later) and newer MacBook Pros (late 2016 and later).

Using it:
---------
This kernel module requires at least Linux 4.14. If you're using an older kernel version, you can refer to an older version of this driver, which still supports older kernel versions.

In all cases make sure you don't have `noapic` in your kernel options.

On the 2015 MacBook make sure the `spi_pxa2xx_platform` and `spi_pxa2xx_pci` kernel modules are loaded (if you don't have those module, rebuild your kernel with `CONFIG_SPI_PXA2XX=m` and `CONFIG_SPI_PXA2XX_PCI=m`).

On all other MacBook's and MacBook Pro's you instead need to make sure both the `spi_pxa2xx_platform` and `intel_lpss_pci` modules are loaded (if these don't exist, you need to (re)compile your kernel with `CONFIG_SPI_PXA2XX=m` and `CONFIG_MFD_INTEL_LPSS_PCI=m`).

For best results everywhere, make sure all three modules (this `applespi` driver plus the two core ones mentioned above) are present in your initramfs/initrd so that the keyboard is functional by the time the prompt for the disk password appears. Also, having them loaded early also appears to remove the need for the `irqpoll` kernel parameter on MacBook8,1's.


DKMS module (Debian & co):
--------------------------
As root, do the following (all MacBook's and MacBook Pro's except MacBook8,1 (2015)):
```
echo -e "\n# applespi\napplespi\nspi_pxa2xx_platform\nintel_lpss_pci" >> /etc/initramfs-tools/modules

apt install dkms
git clone https://github.com/cb22/macbook12-spi-driver.git /usr/src/applespi-0.1
dkms install -m applespi -v 0.1
```

If you're on a MacBook8,1 (2015):
```
echo -e "\n# applespi\napplespi\nspi_pxa2xx_platform\nspi_pxa2xx_pci" >> /etc/initramfs-tools/modules

apt install dkms
git clone https://github.com/cb22/macbook12-spi-driver.git /usr/src/applespi-0.1
dkms install -m applespi -v 0.1
```

What works:
-----------
* Basic Typing
* FN keys
* Driver unloading (no more hanging)
* Basic touchpad functionality (even right click, handled by libinput)
* MT touchpad functionality (two finger scroll, probably others)
* Interrupts!
* Suspend / resume

What doesn't work:
------------------
* Key rollover (properly)
* Wakeup on keypress / touchpad
 
Known bugs:
-----------
* Occasionally, the SPI device can get itself into a state where it causes an interrupt storm. There should be a way of resetting it, or better yet avoiding this state altogether.

Interrupts:
-----------
Interrupts are now working! This means that the driver is no longer polled, and should no longer be a massive battery drain. For more information on how the driver receives interrupts, see the discussion [here](https://github.com/cb22/macbook12-spi-driver/pull/1)

Touchpad:
---------
The touchpad protocol is the same as the bcm5974 driver. Perhaps there is a nice way of utilizing it? For now, bits of code have just been copy and pasted.

Debugging:
----------
The `debug` module parameter can be used to turn debugging output on (and off) dynamically, and can be set in all the usual ways (e.g. via kernel command-line (`applespi.debug=0x1`), via sysfs (`echo 0x10000 | sudo tee /sys/module/applespi/parameters/debug`), etc.).

Some useful values are (since the value is a bitmask, these can be combined):
* 0x10000 - determine touchpad values range
* 0x1     - turn on logging of touchpad initialization packets
* 0x6     - turn on logging of backlight and caps-lock-led packets

Some useful threads:
--------------------
* https://bugzilla.kernel.org/show_bug.cgi?id=108331
* https://bugzilla.kernel.org/show_bug.cgi?id=99891
