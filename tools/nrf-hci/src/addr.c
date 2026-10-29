/* The controller's public address.
 *
 * Zephyr's link layer boots with the public address 00:00:00:00:00:00, and only a Zephyr vendor
 * command, which Linux never sends, can set one. The kernel refuses to power an adapter that has
 * neither a public nor a static address (-EADDRNOTAVAIL), so bluetoothd could not bring the DK up.
 * The nRF54L15's FICR holds a random static address of the chip's own (the one Zephyr's host uses
 * and VS Read Static Addresses reports); the link layer gets it as its public address before HCI
 * starts. HCI Reset clears only the random address, so it stays for the life of the firmware. It is
 * a static random address worn as public: no IEEE OUI behind it, fine for a bench.
 */
#include <zephyr/init.h>
#include <zephyr/bluetooth/controller.h>
#include <zephyr/bluetooth/hci_vs.h>

/* subsys/bluetooth/controller/hci/hci_internal.h, which is not on an application's include path. */
uint8_t hci_vendor_read_static_addr(struct bt_hci_vs_static_addr addrs[], uint8_t size);

static int public_addr_init(void)
{
	struct bt_hci_vs_static_addr addr;

	if (hci_vendor_read_static_addr(&addr, 1) != 1) {
		return -ENODEV;
	}
	bt_ctlr_set_public_addr(addr.bdaddr.val);
	return 0;
}

SYS_INIT(public_addr_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
