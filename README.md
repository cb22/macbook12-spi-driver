Very simple, work in progress polled input driver for the SPI keyboard / trackpad found on 12" MacBooks. 

Using it:
---------
To get this driver to work on a 2016 12" MacBook, you'll need to boot the kernel with `intremap=nosid`. Also, you'll need to modify your DSDT as described by Leif Liddy at https://bugzilla.kernel.org/attachment.cgi?id=206671. (but don't worry about applying patches. Also see https://wiki.archlinux.org/index.php/DSDT for a quicker way to load custom DSDTs without recompiling)

This should result in the intel-lpss driver attaching itself to the SPI controller, and exposing the `APP000D` device.

The 2015 MacBook seems much more complicated, as the DMA controller isn't built in to the SPI controller. Unfortunately, I don't have a 2015 model to test.

What works:
-----------
* Basic Typing
* Driver unloading (no more hanging)
* Basic touchpad functionality (even right click, handled by libinput)

What doesn't work:
------------------
* Key rollover (properly)
* FN keys (simple enough)
* Interrupts
* Suspend / resume probably
* Multitouch (2 finger scrolling, etc)

Interupts:
----------
Currently, how interrupts work are unknown; so this driver will constantly poll the device every few ms. This works, but results in a pretty big battery drain.

Touchpad:
---------
The touchpad protocol is the same as the bcm5974 driver. Perhaps there is a nice way of utilizing it? For now, bits of code have just been copy and pasted.

Some useful threads:
--------------------
* https://bugzilla.kernel.org/show_bug.cgi?id=108331
* https://bugzilla.kernel.org/show_bug.cgi?id=99891
