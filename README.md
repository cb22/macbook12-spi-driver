Very simple, work in progress input driver for the SPI keyboard / trackpad found on 12" MacBooks. 

Using it:
---------
To get this driver to work on a 2016 12" MacBook or a 2016 MacBook Pro, you'll need to boot the kernel with `intremap=nosid` if you're running a kernel before 4.11. Make sure you don't have `noapic` in your kernel options.

Additionally, you need to make sure the `spi_pxa2xx_platform` and `intel_lpss_pci` modules are loaded. This should result in the intel-lpss driver attaching itself to the SPI controller. 

The 2015 MacBook seems much more complicated, as the DMA controller isn't built in to the SPI controller. Unfortunately, I don't have a 2015 model to test.

DKMS module (Debian & co):
--------------------------
As root, do the following:
```
apt install dkms
git clone https://github.com/cb22/macbook12-spi-driver.git /usr/src/applespi-0.1
dkms install -m applespi -v 0.1

echo -e "\n# applespi\napplespi\nintel_lpss_pci\nspi_pxa2xx_platform" >> /etc/initramfs-tools/modules
update-initramfs -u
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

Some useful threads:
--------------------
* https://bugzilla.kernel.org/show_bug.cgi?id=108331
* https://bugzilla.kernel.org/show_bug.cgi?id=99891
