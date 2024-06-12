
====== Policy ======

* constantly rebase to the most recent tag of linus' tree (git push --force)

====== Version ======

* based on : 6.10-rc3

====== Drivers ======

* dts basic:
* uart/serial:
* irqchip
* regulator
* reset
* clock
* pinctrl
* gpio
* i2c -> mfd
* pm domain
* soc (pm domain / reboot control / dma range)
* dmaengine
* mmc/sdhc
* ethernet
* pcie: nvme
* usb: input: mouse/keyboard
* usb: mass storage device
* wathdog
* thermal

====== Bootargs ======

 earlycon=pxa_serial,0xd4017000  earlyprintk console=ttyS0,115200 loglevel=8 root=/dev/mmcblk2p3 rootwait rootfstype=ext4 regulator_ignore_unused clk_ignore_unused


====== Known issue ======
