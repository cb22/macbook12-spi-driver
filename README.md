Very simple, work in progress input driver for the SPI keyboard / trackpad found on 12" MacBooks. 

Using it:
---------
To get this driver to work on a 2016 12" MacBook or a 2016 MacBook Pro, you'll need to boot the kernel with `intremap=nosid`. Additionally, you need to make sure the `spi_pxa2xx_platform` and `intel_lpss_pci` modules are loaded. This should result in the intel-lpss driver attaching itself to the SPI controller.

The 2015 MacBook seems much more complicated, as the DMA controller isn't built in to the SPI controller. Unfortunately, I don't have a 2015 model to test.

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
