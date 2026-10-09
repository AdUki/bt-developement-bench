#!/usr/bin/env python3
"""Self-test for the fake: a scripted BlueZ client, run against it on its private bus.

    tools/fake-bluez/run python3 tools/fake-bluez/test_fake.py

It plays the part btbenchd will: registers a pairing agent, scans, pairs, connects, watches the
media endpoints and transports appear, answers the questions an incoming pairing asks, forgets a
device, walks and reads a GATT database, and registers an advertisement and a GATT application
of its own. Every call is asynchronous, because the agent (and the exported objects) have to
answer the fake while a call of ours is still waiting on it — a blocking call would deadlock
exactly the way a real client would.
"""

import os
import sys
import time

import dbus
import dbus.bus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

PROPS = 'org.freedesktop.DBus.Properties'
ADAPTER = 'org.bluez.Adapter1'
DEVICE = 'org.bluez.Device1'
HCI = '/org/bluez/hci0'
PIXEL = '5C:E9:1E:22:40:01'
JBL = 'F8:DF:15:0A:11:3C'
OLD = '00:1A:7D:DA:71:13'
KEYS = 'C7:3B:52:10:9A:1E'
BEACON = 'D4:CA:6E:00:00:01'
HRM = 'C0:FF:EE:00:00:01'

failures = 0


def check(cond, what):
    global failures
    print(('ok    ' if cond else 'FAIL  ') + what, flush=True)
    if not cond:
        failures += 1


def dev(addr):
    return HCI + '/dev_' + addr.replace(':', '_')


def pump(seconds):
    end = time.monotonic() + seconds
    ctx = GLib.MainContext.default()
    while time.monotonic() < end:
        ctx.iteration(False) or time.sleep(0.01)


def wait_for(cond, timeout):
    end = time.monotonic() + timeout
    ctx = GLib.MainContext.default()
    while time.monotonic() < end:
        if cond():
            return True
        ctx.iteration(False) or time.sleep(0.01)
    return cond()


class Agent(dbus.service.Object):
    """Answers everything yes, unless told to refuse; remembers what it was asked."""

    def __init__(self, conn, path):
        super().__init__(conn, path)
        self.calls = []
        self.refuse = False

    def note(self, *call):
        self.calls.append(call)
        print('      agent:', *call, flush=True)
        if self.refuse:
            raise dbus.DBusException('refused', name='org.bluez.Error.Rejected')

    @dbus.service.method('org.bluez.Agent1', in_signature='', out_signature='')
    def Release(self):
        self.note('Release')

    @dbus.service.method('org.bluez.Agent1', in_signature='o', out_signature='s')
    def RequestPinCode(self, d):
        self.note('RequestPinCode', str(d))
        return '1234'

    @dbus.service.method('org.bluez.Agent1', in_signature='ouq', out_signature='')
    def DisplayPasskey(self, d, passkey, entered):
        self.note('DisplayPasskey', str(d), int(passkey), int(entered))

    @dbus.service.method('org.bluez.Agent1', in_signature='ou', out_signature='')
    def RequestConfirmation(self, d, passkey):
        self.note('RequestConfirmation', str(d), int(passkey))

    @dbus.service.method('org.bluez.Agent1', in_signature='o', out_signature='')
    def RequestAuthorization(self, d):
        self.note('RequestAuthorization', str(d))

    @dbus.service.method('org.bluez.Agent1', in_signature='os', out_signature='')
    def AuthorizeService(self, d, uuid):
        self.note('AuthorizeService', str(d), str(uuid))

    @dbus.service.method('org.bluez.Agent1', in_signature='', out_signature='')
    def Cancel(self):
        self.note('Cancel')


class Client:
    def __init__(self, conn):
        self.conn = conn

    def call(self, path, iface, method, sig='', args=(), dest='org.bluez', timeout=90):
        box = {}
        self.conn.call_async(dest, path, iface, method, sig, args,
                             lambda *r: box.setdefault('ok', r),
                             lambda e: box.setdefault('err', e), timeout=timeout)
        wait_for(lambda: box, timeout + 1)
        if 'err' in box:
            raise box['err']
        return box.get('ok', ())

    def error_of(self, *a, **k):
        try:
            self.call(*a, **k)
        except dbus.DBusException as e:
            return e.get_dbus_name(), e.get_dbus_message()
        return None, None

    def get(self, path, iface, name, dest='org.bluez'):
        return self.call(path, PROPS, 'Get', 'ss', (iface, name), dest=dest)[0]

    def set(self, path, iface, name, value):
        return self.call(path, PROPS, 'Set', 'ssv', (iface, name, value))

    def objects(self, dest='org.bluez', root='/'):
        return self.call(root, 'org.freedesktop.DBus.ObjectManager', 'GetManagedObjects',
                         dest=dest)[0]

    def hook(self, method, *args):
        return self.call('/org/btbench/fake', 'org.btbench.Fake1', method,
                         's' * len(args), args)


class Advertisement(dbus.service.Object):
    """An LEAdvertisement1, as btbenchd exports one: the fake reads it back with GetAll."""

    def __init__(self, conn, path, props):
        super().__init__(conn, path)
        self.props = props
        self.released = False

    @dbus.service.method(PROPS, in_signature='s', out_signature='a{sv}')
    def GetAll(self, iface):
        return dbus.Dictionary(self.props, signature='sv')

    @dbus.service.method('org.bluez.LEAdvertisement1', in_signature='', out_signature='')
    def Release(self):
        self.released = True


class App(dbus.service.Object):
    """A one-characteristic GATT application behind an ObjectManager, as btbenchd exports one."""

    def __init__(self, conn, root):
        super().__init__(conn, root)
        self.root = root
        self.svc = root + '/svc0'
        self.chr = self.svc + '/chr0'
        self.value = [1, 2]
        self.chr_obj = AppChr(conn, self.chr, self)

    @dbus.service.method('org.freedesktop.DBus.ObjectManager', in_signature='', out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        return {dbus.ObjectPath(self.svc): {'org.bluez.GattService1': {
                    'UUID': '0000180d-0000-1000-8000-00805f9b34fb', 'Primary': True}},
                dbus.ObjectPath(self.chr): {'org.bluez.GattCharacteristic1': {
                    'UUID': '00002a37-0000-1000-8000-00805f9b34fb', 'Service': dbus.ObjectPath(self.svc),
                    'Flags': dbus.Array(['read', 'write'], signature='s')}}}


class AppChr(dbus.service.Object):
    def __init__(self, conn, path, app):
        super().__init__(conn, path)
        self.app = app

    @dbus.service.method('org.bluez.GattCharacteristic1', in_signature='a{sv}', out_signature='ay')
    def ReadValue(self, opts):
        return dbus.Array(self.app.value, signature='y')

    @dbus.service.method('org.bluez.GattCharacteristic1', in_signature='aya{sv}', out_signature='')
    def WriteValue(self, value, opts):
        self.app.value = [int(b) for b in value]


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    conn = dbus.bus.BusConnection(os.environ['DBUS_SYSTEM_BUS_ADDRESS'])
    c = Client(conn)
    agent = Agent(conn, '/test/agent')

    # -- the agent ------------------------------------------------------------------------------
    c.call('/org/bluez', 'org.bluez.AgentManager1', 'RegisterAgent', 'os',
           (dbus.ObjectPath('/test/agent'), 'DisplayYesNo'))
    c.call('/org/bluez', 'org.bluez.AgentManager1', 'RequestDefaultAgent', 'o',
           (dbus.ObjectPath('/test/agent'),))
    name, _ = c.error_of('/org/bluez', 'org.bluez.AgentManager1', 'RegisterAgent', 'os',
                         (dbus.ObjectPath('/test/agent'), 'DisplayYesNo'))
    check(name == 'org.bluez.Error.AlreadyExists', 'a second RegisterAgent is AlreadyExists')

    objs = c.objects()
    check(dev('A0:B1:C2:D3:E4:F5') in objs, 'Galaxy Buds known at start')
    check(objs[dev('A0:B1:C2:D3:E4:F5')][DEVICE]['Paired'], 'and paired')
    check('org.freedesktop.DBus.Properties' in objs[HCI], 'managed objects list Properties, as BlueZ does')
    check(objs[HCI]['org.bluez.LEAdvertisingManager1']['SupportedInstances'] == 4,
          'the adapter offers 4 advertising instances')

    # -- discovery ------------------------------------------------------------------------------
    c.call(HCI, ADAPTER, 'SetDiscoveryFilter', 'a{sv}', ({'Transport': 'bredr'},))
    c.call(HCI, ADAPTER, 'StartDiscovery')
    name, _ = c.error_of(HCI, ADAPTER, 'StartDiscovery')
    check(name == 'org.bluez.Error.InProgress', 'a second StartDiscovery is InProgress')
    check(wait_for(lambda: c.get(HCI, ADAPTER, 'Discovering'), 1), 'Discovering goes true')
    check(wait_for(lambda: dev(PIXEL) in c.objects(), 5), 'the Pixel appears during the scan')
    check('RSSI' in c.objects()[dev(PIXEL)][DEVICE], 'with an RSSI')
    pump(2.5)
    check(dev(BEACON) not in c.objects(), 'an LE-only beacon stays hidden under a bredr filter')
    check(dev(HRM) not in c.objects(), 'and so does the LE heart-rate monitor')

    # -- outgoing pairing: numeric comparison -------------------------------------------------
    c.call(dev(PIXEL), DEVICE, 'Pair')
    check(any(x[0] == 'RequestConfirmation' and x[1] == dev(PIXEL) for x in agent.calls),
          'pairing the Pixel asks the agent RequestConfirmation')
    check(c.get(dev(PIXEL), DEVICE, 'Paired'), 'Pixel paired')
    name, msg = c.error_of(dev(PIXEL), DEVICE, 'Pair')
    check(name == 'org.bluez.Error.AlreadyExists', 'pairing it again is AlreadyExists')

    c.call(dev(PIXEL), DEVICE, 'Connect')
    check(c.get(dev(PIXEL), DEVICE, 'Connected'), 'Pixel connected')
    tr = dev(PIXEL) + '/sep1/fd0'
    o = c.objects()
    check(dev(PIXEL) + '/sep1' in o and 'org.bluez.MediaEndpoint1' in o[dev(PIXEL) + '/sep1'],
          'the phone brings its stream endpoint')
    check(tr in o and o[tr]['org.bluez.MediaTransport1']['UUID'] == '0000110b-0000-1000-8000-00805f9b34fb',
          'and a transport where the bench is the sink')
    check(wait_for(lambda: c.get(tr, 'org.bluez.MediaTransport1', 'State') == 'active', 4),
          'which goes active when the phone starts playing')
    check(dev(PIXEL) + '/player0' in c.objects(), 'with an AVRCP player')

    # -- outgoing pairing: just works ---------------------------------------------------------
    check(wait_for(lambda: dev(JBL) in c.objects(), 3), 'the JBL is there')
    before = len(agent.calls)
    c.call(dev(JBL), DEVICE, 'Pair')
    check(len(agent.calls) == before, 'a just-works pairing we start asks the agent nothing')
    c.call(dev(JBL), DEVICE, 'Connect')
    jtr = dev(JBL) + '/sep1/fd0'
    o = c.objects()
    check(jtr in o and o[jtr]['org.bluez.MediaTransport1']['State'] == 'idle'
          and list(o[jtr]['org.bluez.MediaTransport1']['Configuration']) == [0x11, 0x15, 0x02, 0x35],
          'the JBL gets an idle SBC 48 kHz transport')
    check(dev(JBL) + '/sep2' in o, 'and offers AAC too')
    c.set(jtr, 'org.bluez.MediaTransport1', 'Volume', dbus.UInt16(64))
    check(c.get(jtr, 'org.bluez.MediaTransport1', 'Volume') == 64, 'its volume is writable')
    c.hook('StartStream', JBL)
    check(c.get(jtr, 'org.bluez.MediaTransport1', 'State') == 'active', 'start-stream makes it active')

    c.call(HCI, ADAPTER, 'StopDiscovery')
    pump(0.3)
    name, _ = c.error_of(dev(PIXEL), PROPS, 'Get', 'ss', (DEVICE, 'RSSI'))
    check(name == 'org.freedesktop.DBus.Error.InvalidArgs', 'RSSI goes away when the scan stops')
    name, msg = c.error_of(HCI, ADAPTER, 'StopDiscovery')
    check(name == 'org.bluez.Error.Failed' and msg == 'No discovery started',
          'stopping a scan we do not have is Failed: No discovery started')

    # -- LE: the heart-rate monitor's GATT database ---------------------------------------------
    c.call(HCI, ADAPTER, 'SetDiscoveryFilter', 'a{sv}', ({'Transport': 'le'},))
    c.call(HCI, ADAPTER, 'StartDiscovery')
    check(wait_for(lambda: dev(HRM) in c.objects(), 3), 'an LE scan finds the HRM')
    h = c.objects()[dev(HRM)][DEVICE]
    check(h['AddressType'] == 'random' and h['Appearance'] == 0x0341, 'random address, HR appearance')
    check(list(h['ManufacturerData'][0x0059]) == [1, 2], 'with manufacturer data')
    c.call(HCI, ADAPTER, 'StopDiscovery')
    c.call(dev(HRM), DEVICE, 'Connect')
    check(wait_for(lambda: c.get(dev(HRM), DEVICE, 'ServicesResolved'), 2), 'its services resolve')
    hrm = dev(HRM) + '/service000a/char000b'
    check(hrm in c.objects(), 'the Heart Rate Measurement characteristic is there')
    v = c.call(dev(HRM) + '/service0014/char0015', 'org.bluez.GattCharacteristic1', 'ReadValue', 'a{sv}', ({},))[0]
    check(bytes(v) == b'btbench fake', 'reading the manufacturer name')
    name, _ = c.error_of(hrm, 'org.bluez.GattCharacteristic1', 'ReadValue', 'a{sv}', ({},))
    check(name == 'org.bluez.Error.NotPermitted', 'a notify-only characteristic cannot be read')
    seen = []
    conn.add_signal_receiver(lambda i, ch, inv: 'Value' in ch and seen.append(list(ch['Value'])),
                             'PropertiesChanged', PROPS, None, hrm)
    c.call(hrm, 'org.bluez.GattCharacteristic1', 'StartNotify')
    check(wait_for(lambda: len(seen) >= 2, 3), 'notifications tick once a second')
    c.call(hrm, 'org.bluez.GattCharacteristic1', 'StopNotify')
    echo = dev(HRM) + '/service001b/char001c'
    c.call(echo, 'org.bluez.GattCharacteristic1', 'WriteValue', 'aya{sv}', (dbus.ByteArray(b'hi'), {}))
    v = c.call(echo, 'org.bluez.GattCharacteristic1', 'ReadValue', 'a{sv}', ({},))[0]
    check(bytes(v) == b'hi', 'the echo characteristic reads back what was written')
    c.call(dev(HRM), DEVICE, 'Disconnect')
    check(hrm not in c.objects(), 'its GATT objects go when it disconnects')

    # -- LE advertising ---------------------------------------------------------------------------
    adv = Advertisement(conn, '/test/adv0', {'Type': 'peripheral', 'LocalName': 'test',
                                             'ServiceUUIDs': dbus.Array(['180d'], signature='s')})
    c.call(HCI, 'org.bluez.LEAdvertisingManager1', 'RegisterAdvertisement', 'oa{sv}',
           (dbus.ObjectPath('/test/adv0'), {}))
    check(c.get(HCI, 'org.bluez.LEAdvertisingManager1', 'ActiveInstances') == 1, 'one advertisement active')
    check(c.get(HCI, 'org.bluez.LEAdvertisingManager1', 'SupportedInstances') == 3, 'three instances left')
    check('LocalName=test' in c.hook('Advertisements')[0], 'the fake read it back with GetAll')
    name, _ = c.error_of(HCI, 'org.bluez.LEAdvertisingManager1', 'RegisterAdvertisement', 'oa{sv}',
                         (dbus.ObjectPath('/test/adv0'), {}))
    check(name == 'org.bluez.Error.AlreadyExists', 'registering it twice is AlreadyExists')
    bad = Advertisement(conn, '/test/adv1', {'Type': 'beacon'})
    name, _ = c.error_of(HCI, 'org.bluez.LEAdvertisingManager1', 'RegisterAdvertisement', 'oa{sv}',
                         (dbus.ObjectPath('/test/adv1'), {}))
    check(name == 'org.bluez.Error.Failed', 'a bad Type is Failed')
    short = Advertisement(conn, '/test/adv2', {'Type': 'broadcast', 'Timeout': dbus.UInt16(1)})
    c.call(HCI, 'org.bluez.LEAdvertisingManager1', 'RegisterAdvertisement', 'oa{sv}',
           (dbus.ObjectPath('/test/adv2'), {}))
    check(wait_for(lambda: short.released, 3), 'an advertisement with a Timeout is Released')
    c.call(HCI, 'org.bluez.LEAdvertisingManager1', 'UnregisterAdvertisement', 'o', (dbus.ObjectPath('/test/adv0'),))
    check(c.get(HCI, 'org.bluez.LEAdvertisingManager1', 'ActiveInstances') == 0, 'and unregistered')
    del adv, bad

    # -- a GATT application -------------------------------------------------------------------
    App(conn, '/test/app')
    c.call(HCI, 'org.bluez.GattManager1', 'RegisterApplication', 'oa{sv}', (dbus.ObjectPath('/test/app'), {}))
    check(c.hook('AppRead', '2a37')[0] == '0102', 'a remote read reaches the application')
    c.hook('AppWrite', '2a37', '0a0b')
    check(c.hook('AppRead', '2a37')[0] == '0a0b', 'and a remote write')
    c.call(HCI, 'org.bluez.GattManager1', 'UnregisterApplication', 'o', (dbus.ObjectPath('/test/app'),))

    # -- incoming pairing: a legacy PIN, then a service authorisation -------------------------
    r = c.hook('IncomingPair', OLD)[0]
    check(r == 'paired+authorized+connected', 'the old speaker pairs with PIN 1234: ' + r)
    check(any(x[0] == 'RequestPinCode' for x in agent.calls), 'the agent was asked for a PIN')
    check(any(x[0] == 'AuthorizeService' for x in agent.calls), 'then to authorise A2DP')

    # -- incoming pairing, refused ------------------------------------------------------------
    c.call(HCI, ADAPTER, 'RemoveDevice', 'o', (dbus.ObjectPath(dev(PIXEL)),))
    check(dev(PIXEL) not in c.objects(), 'RemoveDevice forgets the Pixel')
    check(tr not in c.objects(), 'and its transport goes with it')
    agent.refuse = True
    r = c.hook('IncomingPair', PIXEL)[0]
    agent.refuse = False
    check(r == 'pairing failed: org.bluez.Error.AuthenticationRejected',
          'a refused confirmation is AuthenticationRejected: ' + r)

    # -- displayed passkey ----------------------------------------------------------------------
    n = len(agent.calls)
    r = c.hook('Display', KEYS)[0]
    shown = [x for x in agent.calls[n:] if x[0] == 'DisplayPasskey']
    check(r == 'paired' and len(shown) == 7 and shown[-1][3] == 6,
          'DisplayPasskey counts the typed digits up to six')

    # -- the link drops -------------------------------------------------------------------------
    c.hook('DropLink', JBL)
    check(not c.get(dev(JBL), DEVICE, 'Connected') and jtr not in c.objects(),
          'DropLink disconnects the JBL and drops its transport')
    name, msg = c.error_of(dev(JBL), DEVICE, 'Connect')
    check(name == 'org.bluez.Error.Failed' and msg == 'br-connection-page-timeout',
          'reconnecting out of range is Failed: br-connection-page-timeout')

    # -- discoverable, and its timeout --------------------------------------------------------
    c.set(HCI, ADAPTER, 'DiscoverableTimeout', dbus.UInt32(3))
    c.set(HCI, ADAPTER, 'Discoverable', dbus.Boolean(True))
    check(c.get(HCI, ADAPTER, 'Discoverable'), 'discoverable')
    check(wait_for(lambda: not c.get(HCI, ADAPTER, 'Discoverable'), 5),
          'and not any more once the 3 s timeout runs out')

    # -- power ----------------------------------------------------------------------------------
    c.set(HCI, ADAPTER, 'Powered', dbus.Boolean(False))
    check(not c.get(dev(OLD), DEVICE, 'Connected'), 'powering off disconnects everything')
    name, msg = c.error_of(HCI, PROPS, 'Set', 'ssv', (ADAPTER, 'Discoverable', dbus.Boolean(True)))
    check(name == 'org.bluez.Error.Failed' and msg == 'Not Powered',
          'discoverable while off is Failed: Not Powered')
    name, _ = c.error_of(HCI, PROPS, 'Set', 'ssv', (ADAPTER, 'Address', 'x'))
    check(name == 'org.freedesktop.DBus.Error.PropertyReadOnly', 'Address is read-only')
    c.set(HCI, ADAPTER, 'Powered', dbus.Boolean(True))

    c.hook('Reset')
    check(set(c.objects()) == {'/org/bluez', HCI, dev('A0:B1:C2:D3:E4:F5')},
          'Reset puts the initial world back')

    print('PASS' if failures == 0 else 'FAILED: %d check(s)' % failures, flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
