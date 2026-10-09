#!/usr/bin/env python3
"""A fake BlueZ 5.7x for a private D-Bus bus.

btbenchd talks to org.bluez for everything: the adapter, discovery, the devices, the pairing agent,
the GATT client, LE advertising, a local GATT application and the media endpoints and transports.
None of it can be exercised on a laptop without taking over that laptop's own radio, and pairing
cannot be exercised at all without a phone in the hand. This plays org.bluez on a bus of its own,
with a small scripted world of devices, so the daemon and the console can be driven end to end
from a desk.

It is faithful where the daemon could tell the difference: object paths, interface and property
names, D-Bus types, which errors come back and with what name, which calls go to the pairing agent
and with what arguments, that the slow operations answer late, and that it reads an advertisement
(GetAll) and a GATT application (GetManagedObjects) from the client the way BlueZ does. It is not
a radio: no audio flows and no packet is ever sent.

Run it through ./run, which starts the bus. Test hooks live on /org/btbench/fake
(org.btbench.Fake1); ./ctl wraps them.
"""

import argparse
import random
import sys
import time

import dbus
import dbus.bus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

PROPS = 'org.freedesktop.DBus.Properties'
OBJMGR = 'org.freedesktop.DBus.ObjectManager'
INTROSPECT = 'org.freedesktop.DBus.Introspectable'
ADAPTER = 'org.bluez.Adapter1'
DEVICE = 'org.bluez.Device1'
AGENT_MGR = 'org.bluez.AgentManager1'
AGENT = 'org.bluez.Agent1'
ADV_MGR = 'org.bluez.LEAdvertisingManager1'
ADV = 'org.bluez.LEAdvertisement1'
GATT_MGR = 'org.bluez.GattManager1'
GATT_SVC = 'org.bluez.GattService1'
GATT_CHR = 'org.bluez.GattCharacteristic1'
GATT_DSC = 'org.bluez.GattDescriptor1'
ENDPOINT = 'org.bluez.MediaEndpoint1'
TRANSPORT = 'org.bluez.MediaTransport1'
PLAYER = 'org.bluez.MediaPlayer1'
FOLDER = 'org.bluez.MediaFolder1'
ITEM = 'org.bluez.MediaItem1'
FAKE = 'org.btbench.Fake1'

# The adapter the scripted world is in range of. --adapters 2 moves it to hci1 and puts an
# adapter with nothing in range at hci0, as on a desktop whose own radio is the desktop's and whose
# second one is the bench's (btbenchd --bt-adapter).
ADAPTER_PATH = '/org/bluez/hci0'
DESK_ADAPTER = ('/org/bluez/hci0', '44:A3:BB:36:5E:2E')
# What a BCM43430A1 offers: LE advertising sets, as HCI_LE_Read_Number_of_Supported_Advertising_Sets
# would not say on a 4.1 controller; BlueZ falls back to software rotation of up to this many.
ADV_INSTANCES = 4

UUID_AUDIO_SOURCE = '0000110a-0000-1000-8000-00805f9b34fb'
UUID_AUDIO_SINK = '0000110b-0000-1000-8000-00805f9b34fb'
UUID_AVRCP_TARGET = '0000110c-0000-1000-8000-00805f9b34fb'
UUID_A2DP = '0000110d-0000-1000-8000-00805f9b34fb'
UUID_AVRCP = '0000110e-0000-1000-8000-00805f9b34fb'
UUID_HFP = '0000111e-0000-1000-8000-00805f9b34fb'
UUID_HID = '00001124-0000-1000-8000-00805f9b34fb'
UUID_PNP = '00001200-0000-1000-8000-00805f9b34fb'


def u16(v):
    return '0000%04x-0000-1000-8000-00805f9b34fb' % v


UUID_HRS, UUID_BAS, UUID_DIS = u16(0x180d), u16(0x180f), u16(0x180a)
UUID_ECHO_SVC = '12345678-1234-5678-1234-56789abcdef0'
UUID_ECHO_CHR = '12345678-1234-5678-1234-56789abcdef1'

# BlueZ's agent timeout (src/agent.c REQUEST_TIMEOUT): a question nobody answers fails after this.
AGENT_TIMEOUT_S = 60
# How long a device found by a scan outlives the scan, as BlueZ's TemporaryTimeout does.
TEMPORARY_TIMEOUT_S = 30
# How long DropLink keeps a device out of reach before it can be connected again.
OUT_OF_RANGE_S = 15


def log(*args):
    print(time.strftime('%H:%M:%S'), *args, flush=True)


def err(name, msg):
    """An error as BlueZ names it: org.bluez.Error.<name>, with BlueZ's own message."""
    return dbus.DBusException(msg, name='org.bluez.Error.' + name)


def hci_name():
    return ADAPTER_PATH.rsplit('/', 1)[1]


def dev_path(address):
    return ADAPTER_PATH + '/dev_' + address.replace(':', '_')


def strv(items):
    return dbus.Array(items, signature='s')


def ay(items):
    return dbus.Array([dbus.Byte(b) for b in items], signature='y')


def empty():
    return dbus.Dictionary({}, signature='sv')


def signature_of(v):
    for t, sig in ((dbus.Boolean, 'b'), (dbus.Byte, 'y'), (dbus.Int16, 'n'), (dbus.UInt16, 'q'),
                   (dbus.UInt32, 'u'), (dbus.ObjectPath, 'o'), (dbus.String, 's')):
        if isinstance(v, t):
            return sig
    if isinstance(v, dbus.Array):
        return 'a' + (v.signature or 's')
    return 'v'


def hexs(b):
    return ''.join('%02x' % int(x) for x in b)


# ---- the world ----------------------------------------------------------------------------------
#
# What a scan can find. `pairing` says how the device pairs: 'justworks' (no question asked when
# we start it; RequestAuthorization when it starts it), 'confirm' (numeric comparison: both sides
# show a passkey), 'pin' (a legacy device: RequestPinCode), 'display' (we show a passkey the other
# side types: DisplayPasskey). `audio` is the A2DP role a connection gives it: 'playback' for a
# speaker we stream to, 'capture' for a phone streaming to us — each brings its remote stream
# endpoints and a transport (MediaEndpoint1/MediaTransport1). `gatt` means it has a GATT database
# that is resolved when it connects.

WORLD = [
    dict(address='D4:CA:6E:00:00:01', name='LE-only beacon', address_type='random', icon=None,
         cls=None, uuids=[], pairing='justworks', audio=None, rssi=-88, appears=0.5,
         le_only=True, mfr={0x004c: [0x02, 0x15] + list(range(16)) + [0, 1, 0, 2, 0xc5]},
         adv_flags=0x06),
    dict(address='C0:FF:EE:00:00:01', name='Bench HRM', address_type='random', icon=None,
         cls=None, uuids=[UUID_HRS, UUID_BAS, UUID_DIS], pairing='justworks', audio=None,
         gatt=True, rssi=-61, appears=0.8, le_only=True, appearance=0x0341, tx_power=4,
         mfr={0x0059: [0x01, 0x02]}, svc_data={UUID_HRS: [0x48]}, adv_flags=0x06),
    dict(address='F8:DF:15:0A:11:3C', name='JBL Flip 5', icon='audio-card', cls=0x240414,
         uuids=[UUID_AUDIO_SINK, UUID_AVRCP_TARGET, UUID_AVRCP, UUID_HFP, UUID_PNP],
         pairing='justworks', audio='playback', rate=48000, rssi=-58, appears=1.0),
    dict(address='5C:E9:1E:22:40:01', name='Pixel 7', icon='phone', cls=0x5a020c,
         uuids=[UUID_AUDIO_SOURCE, UUID_AVRCP_TARGET, UUID_AVRCP, UUID_PNP],
         pairing='confirm', audio='capture', rate=44100, rssi=-45, appears=2.0),
    dict(address='C7:3B:52:10:9A:1E', name='MX Keys', icon='input-keyboard', cls=0x002540,
         uuids=[UUID_HID, UUID_PNP], pairing='display', audio=None, rssi=-70, appears=3.0),
    dict(address='00:1A:7D:DA:71:13', name='Old Speaker', icon='audio-card', cls=0x240414,
         uuids=[UUID_AUDIO_SINK], pairing='pin', pin='1234', legacy=True, audio='playback',
         rate=44100, rssi=-77, appears=4.0),
    dict(address='A0:B1:C2:D3:E4:F5', name='Galaxy Buds', icon='audio-headphones', cls=0x240404,
         uuids=[UUID_AUDIO_SINK, UUID_AVRCP_TARGET, UUID_AVRCP, UUID_HFP], pairing='confirm',
         audio='playback', rate=48000, rssi=-66, appears=1.5, known=True),
]


def world_entry(address):
    for w in WORLD:
        if w['address'].upper() == address.upper():
            return w
    return None


# ---- property plumbing --------------------------------------------------------------------------


def player_text(props):
    """A player's PropertiesChanged or RegisterPlayer dict, as one line."""
    md = props.get('Metadata', {})
    artist = ', '.join(str(a) for a in md.get('xesam:artist', [])) or '-'
    return '%s "%s" by %s (%s)' % (props.get('PlaybackStatus', '-'), md.get('xesam:title', '-'),
                                   artist, md.get('xesam:album', ''))

class PropObject(dbus.service.Object):
    """An exported object with BlueZ-style properties: GetAll/Get/Set and PropertiesChanged.

    `writable` maps "iface.prop" to a setter that may raise a D-Bus error, which is how a
    read-only property, a bad value or a refused change come back the way BlueZ sends them."""

    def __init__(self, svc, path, ifaces, writable=None, extra_ifaces=()):
        super().__init__(svc.conn, path)
        self.svc = svc
        self.path = path
        self.ifaces = ifaces          # iface -> {name: dbus value}
        self.writable = writable or {}
        # BlueZ lists Introspectable and Properties with empty dicts in GetManagedObjects; GDBus
        # (bluez-alsa) does not. Mirror each, so a client that trips on either is caught here.
        self.extra_ifaces = extra_ifaces

    def managed(self):
        d = dbus.Dictionary({}, signature='sa{sv}')
        for x in self.extra_ifaces:
            d[x] = empty()
        for iface, props in self.ifaces.items():
            d[iface] = dbus.Dictionary(props, signature='sv')
        return d

    def update(self, iface, **changes):
        """Sets properties and announces the ones that actually changed."""
        props = self.ifaces[iface]
        changed = {}
        for k, v in changes.items():
            if k not in props or props[k] != v or type(props[k]) is not type(v):
                props[k] = v
                changed[k] = v
        if changed:
            self.PropertiesChanged(iface, dbus.Dictionary(changed, signature='sv'), strv([]))

    def invalidate(self, iface, *names):
        props = self.ifaces[iface]
        gone = [n for n in names if n in props]
        for n in gone:
            del props[n]
        if gone:
            self.PropertiesChanged(iface, empty(), strv(gone))

    # dbus-python introspects only methods and signals; add the properties, so that busctl
    # introspect shows what GetAll would return.
    @dbus.service.method(INTROSPECT, in_signature='', out_signature='s',
                         path_keyword='object_path', connection_keyword='connection')
    def Introspect(self, object_path, connection):
        xml = dbus.service.Object.Introspect(self, object_path, connection)
        for iface, props in self.ifaces.items():
            rows = ''.join('<property name="%s" type="%s" access="%s"/>' % (
                k, signature_of(v), 'readwrite' if (iface + '.' + k) in self.writable else 'read')
                for k, v in props.items())
            tag = '<interface name="%s">' % iface
            if tag in xml:
                xml = xml.replace(tag, tag + rows, 1)
            else:
                xml = xml.replace('</node>', tag + rows + '</interface></node>', 1)
        return xml

    @dbus.service.method(PROPS, in_signature='ss', out_signature='v')
    def Get(self, iface, name):
        if iface not in self.ifaces:
            raise dbus.DBusException('No such interface ' + iface,
                                     name='org.freedesktop.DBus.Error.InvalidArgs')
        if name not in self.ifaces[iface]:
            raise dbus.DBusException('No such property ' + name,
                                     name='org.freedesktop.DBus.Error.InvalidArgs')
        return self.ifaces[iface][name]

    @dbus.service.method(PROPS, in_signature='s', out_signature='a{sv}')
    def GetAll(self, iface):
        if iface not in self.ifaces:
            raise dbus.DBusException('No such interface ' + iface,
                                     name='org.freedesktop.DBus.Error.InvalidArgs')
        return dbus.Dictionary(self.ifaces[iface], signature='sv')

    @dbus.service.method(PROPS, in_signature='ssv', out_signature='')
    def Set(self, iface, name, value):
        if iface not in self.ifaces or name not in self.ifaces[iface]:
            raise dbus.DBusException('No such property ' + name,
                                     name='org.freedesktop.DBus.Error.InvalidArgs')
        setter = self.writable.get(iface + '.' + name)
        if setter is None:
            raise dbus.DBusException("Property '%s' is not writable" % name,
                                     name='org.freedesktop.DBus.Error.PropertyReadOnly')
        log('Set %s %s.%s = %r' % (self.path, iface, name, value))
        setter(value)

    @dbus.service.signal(PROPS, signature='sa{sv}as')
    def PropertiesChanged(self, iface, changed, invalidated):
        pass


class ObjectManager(dbus.service.Object):
    """GetManagedObjects over a set of PropObjects, with the two signals."""

    def __init__(self, svc, path):
        super().__init__(svc.conn, path)
        self.objects = {}   # path -> PropObject

    def add(self, obj):
        self.objects[obj.path] = obj
        self.InterfacesAdded(dbus.ObjectPath(obj.path), obj.managed())

    def remove(self, obj):
        if self.objects.pop(obj.path, None) is None:
            return
        names = list(obj.extra_ifaces) + list(obj.ifaces.keys())
        self.InterfacesRemoved(dbus.ObjectPath(obj.path), strv(names))
        obj.remove_from_connection()

    @dbus.service.method(OBJMGR, in_signature='', out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        out = dbus.Dictionary({}, signature='oa{sa{sv}}')
        for path, obj in self.objects.items():
            out[dbus.ObjectPath(path)] = obj.managed()
        return out

    @dbus.service.signal(OBJMGR, signature='oa{sa{sv}}')
    def InterfacesAdded(self, path, ifaces):
        pass

    @dbus.service.signal(OBJMGR, signature='oas')
    def InterfacesRemoved(self, path, ifaces):
        pass


# ---- org.bluez ----------------------------------------------------------------------------------

class AgentManager(PropObject):
    def __init__(self, svc):
        super().__init__(svc, '/org/bluez', {AGENT_MGR: {}, 'org.bluez.ProfileManager1': {}},
                         extra_ifaces=(INTROSPECT,))

    @dbus.service.method(AGENT_MGR, in_signature='os', out_signature='', sender_keyword='sender')
    def RegisterAgent(self, path, capability, sender=None):
        caps = ('', 'DisplayOnly', 'DisplayYesNo', 'KeyboardOnly', 'NoInputNoOutput',
                'KeyboardDisplay')
        if capability not in caps:
            raise err('InvalidArguments', 'Invalid arguments in method call')
        if sender in self.svc.agents:
            raise err('AlreadyExists', 'Already Exists')
        self.svc.agents[sender] = (str(path), capability or 'KeyboardDisplay')
        log('agent registered: %s %s (%s)' % (sender, path, capability))

    @dbus.service.method(AGENT_MGR, in_signature='o', out_signature='', sender_keyword='sender')
    def UnregisterAgent(self, path, sender=None):
        a = self.svc.agents.get(sender)
        if not a or a[0] != str(path):
            raise err('DoesNotExist', 'Does Not Exist')
        self.svc.drop_agent(sender)

    @dbus.service.method(AGENT_MGR, in_signature='o', out_signature='', sender_keyword='sender')
    def RequestDefaultAgent(self, path, sender=None):
        a = self.svc.agents.get(sender)
        if not a or a[0] != str(path):
            raise err('DoesNotExist', 'Does Not Exist')
        self.svc.default_agent = sender
        log('default agent: %s %s' % (sender, path))


class Adapter(PropObject):
    def __init__(self, svc, path=None, address='B8:27:EB:50:7B:22', world=True):
        self.world = world   # False: nothing is ever in range of it
        p = {
            'Address': dbus.String(address),
            'AddressType': dbus.String('public'),
            'Name': dbus.String('BlueZ 5.84'),
            'Alias': dbus.String('BlueZ 5.84'),
            'Class': dbus.UInt32(0x000000),
            'Powered': dbus.Boolean(True),
            'PowerState': dbus.String('on'),
            'Discoverable': dbus.Boolean(False),
            'DiscoverableTimeout': dbus.UInt32(180),
            'Pairable': dbus.Boolean(True),
            'PairableTimeout': dbus.UInt32(0),
            'Discovering': dbus.Boolean(False),
            'UUIDs': strv([UUID_AUDIO_SOURCE, UUID_AUDIO_SINK, UUID_AVRCP_TARGET, UUID_AVRCP]),
            'Modalias': dbus.String('usb:v1D6Bp0246d0548'),
            'Roles': strv(['central', 'peripheral']),
        }
        w = {
            ADAPTER + '.Alias': self.set_alias,
            ADAPTER + '.Powered': self.set_powered,
            ADAPTER + '.Discoverable': self.set_discoverable,
            ADAPTER + '.DiscoverableTimeout': self.set_discoverable_timeout,
            ADAPTER + '.Pairable': self.set_pairable,
            ADAPTER + '.PairableTimeout': self.set_pairable_timeout,
        }
        super().__init__(svc, path or ADAPTER_PATH,
                         {ADAPTER: p, 'org.bluez.Media1': {
                             'SupportedUUIDs': strv([UUID_AUDIO_SOURCE, UUID_AUDIO_SINK])},
                          GATT_MGR: {},
                          ADV_MGR: {
                             'ActiveInstances': dbus.Byte(0),
                             'SupportedInstances': dbus.Byte(ADV_INSTANCES),
                             'SupportedIncludes': strv(['tx-power', 'appearance', 'local-name']),
                             'SupportedSecondaryChannels': strv(['1M', '2M']),
                             'SupportedFeatures': strv(['CanSetTxPower', 'HardwareOffload'])}},
                         writable=w, extra_ifaces=(INTROSPECT, PROPS))
        self.disc_timer = None
        self.pair_timer = None
        self.discovery = {}     # sender -> True while that client scans
        self.filters = {}       # sender -> Transport
        self.scan_timers = []
        self.players = set()    # (sender, path) registered with Media1

    @property
    def p(self):
        return self.ifaces[ADAPTER]

    # -- writable properties --

    def set_alias(self, v):
        # An empty alias puts the system name back, as in BlueZ.
        self.update(ADAPTER, Alias=dbus.String(str(v) or str(self.p['Name'])))

    def set_powered(self, v):
        on = bool(v)
        if on == bool(self.p['Powered']):
            return
        if not on:
            # Everything that needs the radio stops with it.
            for sender in list(self.discovery):
                self.discovery.pop(sender)
            self.sync_discovering()
            self.update(ADAPTER, Discoverable=dbus.Boolean(False))
            self.cancel_timer('disc_timer')
            for d in list(self.svc.devices.values()) if self.world else ():
                d.link_lost('adapter powered off')
        self.update(ADAPTER, Powered=dbus.Boolean(on),
                    PowerState=dbus.String('on' if on else 'off'))

    def set_discoverable(self, v):
        on = bool(v)
        if on and not self.p['Powered']:
            raise err('Failed', 'Not Powered')
        self.update(ADAPTER, Discoverable=dbus.Boolean(on))
        self.arm_discoverable()

    def set_discoverable_timeout(self, v):
        self.update(ADAPTER, DiscoverableTimeout=dbus.UInt32(int(v)))
        self.arm_discoverable()

    def set_pairable(self, v):
        self.update(ADAPTER, Pairable=dbus.Boolean(bool(v)))
        self.arm_pairable()

    def set_pairable_timeout(self, v):
        self.update(ADAPTER, PairableTimeout=dbus.UInt32(int(v)))
        self.arm_pairable()

    # -- timeouts --

    def cancel_timer(self, attr):
        t = getattr(self, attr)
        if t is not None:
            GLib.source_remove(t)
            setattr(self, attr, None)

    def arm_discoverable(self):
        # Restarted by any change, the way the kernel restarts its own countdown.
        self.cancel_timer('disc_timer')
        secs = int(self.p['DiscoverableTimeout'])
        if self.p['Discoverable'] and secs > 0:
            def expire():
                self.disc_timer = None
                log('discoverable timeout expired')
                self.update(ADAPTER, Discoverable=dbus.Boolean(False))
                return False
            self.disc_timer = GLib.timeout_add_seconds(secs, expire)

    def arm_pairable(self):
        self.cancel_timer('pair_timer')
        secs = int(self.p['PairableTimeout'])
        if self.p['Pairable'] and secs > 0:
            def expire():
                self.pair_timer = None
                self.update(ADAPTER, Pairable=dbus.Boolean(False))
                return False
            self.pair_timer = GLib.timeout_add_seconds(secs, expire)

    # -- discovery --

    def transport(self):
        """The filter in force. BlueZ merges every client's filter; the last one set will do."""
        t = 'auto'
        for v in self.filters.values():
            t = v
        return t

    def sync_discovering(self):
        on = bool(self.discovery)
        if on == bool(self.p['Discovering']):
            return
        self.update(ADAPTER, Discovering=dbus.Boolean(on))
        if on:
            self.start_world()
        else:
            for t in self.scan_timers:
                GLib.source_remove(t)
            self.scan_timers = []
            for d in list(self.svc.devices.values()) if self.world else ():
                d.discovery_stopped()

    def start_world(self):
        if not self.world:
            return
        t = self.transport()
        for w in WORLD:
            if w.get('le_only') and t == 'bredr':
                continue
            if not w.get('le_only') and t == 'le':
                continue

            tid = []

            def appear(w=w, tid=tid):
                # Fired, so no longer ours to cancel when the scan stops.
                self.scan_timers.remove(tid[0])
                d = self.svc.devices.get(w['address'])
                if d is None:
                    d = self.svc.add_device(w)
                d.seen(w['rssi'])
                return False
            tid.append(GLib.timeout_add(int(w['appears'] * 1000), appear))
            self.scan_timers.append(tid[0])

        def jitter():
            if not self.p['Discovering']:
                return False
            for d in self.svc.devices.values():
                if d.in_range and 'RSSI' in d.p:
                    d.update(DEVICE, RSSI=dbus.Int16(d.w['rssi'] + random.randint(-4, 4)))
            return True
        self.scan_timers.append(GLib.timeout_add(2000, jitter))

    # Media1: the media player a client offers to AVRCP controllers. Logged,
    # and its track again whenever it changes, which is what a speaker would be told.
    @dbus.service.method('org.bluez.Media1', in_signature='oa{sv}', out_signature='',
                         sender_keyword='sender')
    def RegisterPlayer(self, path, props, sender=None):
        if (sender, path) in self.players:
            raise err('AlreadyExists', 'Already Exists')
        self.players.add((sender, path))
        log('RegisterPlayer %s by %s: %s' % (path, sender, player_text(props)))

        def changed(_iface, props, _invalidated):
            log('player %s: %s' % (path, player_text(props)))
        self.svc.conn.add_signal_receiver(changed, 'PropertiesChanged', PROPS, sender, str(path))

    @dbus.service.method('org.bluez.Media1', in_signature='o', out_signature='',
                         sender_keyword='sender')
    def UnregisterPlayer(self, path, sender=None):
        self.players.discard((sender, path))

    @dbus.service.method(ADAPTER, in_signature='', out_signature='', sender_keyword='sender')
    def StartDiscovery(self, sender=None):
        if not self.p['Powered']:
            raise err('NotReady', 'Resource Not Ready')
        if sender in self.discovery:
            raise err('InProgress', 'Operation already in progress')
        log('StartDiscovery by %s (transport %s)' % (sender, self.filters.get(sender, 'auto')))
        self.discovery[sender] = True
        self.sync_discovering()

    @dbus.service.method(ADAPTER, in_signature='', out_signature='', sender_keyword='sender')
    def StopDiscovery(self, sender=None):
        if not self.p['Powered']:
            raise err('NotReady', 'Resource Not Ready')
        if sender not in self.discovery:
            raise err('Failed', 'No discovery started')
        log('StopDiscovery by %s' % sender)
        del self.discovery[sender]
        self.sync_discovering()

    @dbus.service.method(ADAPTER, in_signature='a{sv}', out_signature='', sender_keyword='sender')
    def SetDiscoveryFilter(self, f, sender=None):
        t = str(f.get('Transport', 'auto'))
        if t not in ('auto', 'bredr', 'le'):
            raise err('InvalidArguments', 'Invalid arguments in method call')
        for k in f.keys():
            if k not in ('UUIDs', 'RSSI', 'Pathloss', 'Transport', 'DuplicateData',
                         'Discoverable', 'Pattern'):
                raise err('InvalidArguments', 'Invalid arguments in method call')
        if f:
            self.filters[sender] = t
        else:
            self.filters.pop(sender, None)   # an empty dict clears the client's filter
        log('SetDiscoveryFilter by %s: %s' % (sender, dict(f)))

    @dbus.service.method(ADAPTER, in_signature='', out_signature='as')
    def GetDiscoveryFilters(self):
        return strv(['UUIDs', 'RSSI', 'Pathloss', 'Transport', 'DuplicateData', 'Discoverable',
                     'Pattern'])

    @dbus.service.method(ADAPTER, in_signature='o', out_signature='')
    def RemoveDevice(self, path):
        d = next((x for x in self.svc.devices.values() if x.path == str(path)), None)
        if d is None:
            raise err('DoesNotExist', 'Does Not Exist')
        log('RemoveDevice %s' % d.address)
        self.svc.remove_device(d)

    def client_gone(self, sender):
        self.filters.pop(sender, None)
        if self.discovery.pop(sender, None):
            self.sync_discovering()


class Device(PropObject):
    def __init__(self, svc, w):
        self.w = w
        self.address = w['address']
        p = {
            'Address': dbus.String(w['address']),
            'AddressType': dbus.String(w.get('address_type', 'public')),
            'Name': dbus.String(w['name']),
            'Alias': dbus.String(w['name']),
            'Paired': dbus.Boolean(bool(w.get('known'))),
            'Bonded': dbus.Boolean(bool(w.get('known'))),
            'Trusted': dbus.Boolean(bool(w.get('known'))),
            'Blocked': dbus.Boolean(False),
            'LegacyPairing': dbus.Boolean(bool(w.get('legacy'))),
            'Connected': dbus.Boolean(False),
            'ServicesResolved': dbus.Boolean(False),
            'UUIDs': strv(w['uuids']),
            'Adapter': dbus.ObjectPath(ADAPTER_PATH),
            'WakeAllowed': dbus.Boolean(False),
        }
        if w.get('icon'):
            p['Icon'] = dbus.String(w['icon'])
        if w.get('appearance') is not None:
            p['Appearance'] = dbus.UInt16(w['appearance'])
        if w.get('tx_power') is not None:
            p['TxPower'] = dbus.Int16(w['tx_power'])
        if w.get('mfr'):
            p['ManufacturerData'] = dbus.Dictionary(
                {dbus.UInt16(k): dbus.Array(v, signature='y') for k, v in w['mfr'].items()},
                signature='qv')
        if w.get('svc_data'):
            p['ServiceData'] = dbus.Dictionary(
                {k: dbus.Array(v, signature='y') for k, v in w['svc_data'].items()}, signature='sv')
        if w.get('adv_flags') is not None:
            p['AdvertisingFlags'] = dbus.Array([w['adv_flags']], signature='y')
        if w.get('cls') is not None:
            p['Class'] = dbus.UInt32(w['cls'])
        wr = {
            DEVICE + '.Alias': self.set_alias,
            DEVICE + '.Trusted': self.set_trusted,
            DEVICE + '.Blocked': self.set_blocked,
            DEVICE + '.WakeAllowed': lambda v: self.update(DEVICE, WakeAllowed=dbus.Boolean(v)),
        }
        super().__init__(svc, dev_path(w['address']), {DEVICE: p}, writable=wr,
                         extra_ifaces=(INTROSPECT, PROPS))
        self.in_range = True
        self.range_timer = None
        self.forget_timer = None
        self.busy = None          # 'pair' | 'connect' while one of them runs
        self.attempt = 0          # bumped to orphan a late agent reply after a cancel
        self.pending_pair = None  # (ok, err) of an outstanding Pair(), for CancelPairing
        self.agent_waiting = None  # the agent (sender, path) we are waiting on, if any

    @property
    def p(self):
        return self.ifaces[DEVICE]

    def set_alias(self, v):
        self.update(DEVICE, Alias=dbus.String(str(v) or str(self.p['Name'])))

    def set_trusted(self, v):
        self.update(DEVICE, Trusted=dbus.Boolean(bool(v)))

    def set_blocked(self, v):
        self.update(DEVICE, Blocked=dbus.Boolean(bool(v)))
        if v:
            self.link_lost('blocked')

    # -- presence --

    def seen(self, rssi):
        self.cancel_forget()
        if self.in_range:
            self.update(DEVICE, RSSI=dbus.Int16(rssi))

    def discovery_stopped(self):
        # RSSI is only meaningful while scanning: BlueZ invalidates it when the scan ends, and
        # forgets a device the scan found unless it was paired or connected meanwhile.
        self.invalidate(DEVICE, 'RSSI')
        if not self.p['Paired'] and not self.p['Connected'] and not self.p['Trusted']:
            def forget():
                self.forget_timer = None
                if not self.p['Paired'] and not self.p['Connected'] and self.busy is None:
                    log('temporary device %s expired' % self.address)
                    self.svc.remove_device(self)
                return False
            self.cancel_forget()
            self.forget_timer = GLib.timeout_add_seconds(TEMPORARY_TIMEOUT_S, forget)

    def cancel_forget(self):
        if self.forget_timer is not None:
            GLib.source_remove(self.forget_timer)
            self.forget_timer = None

    # -- the link --

    def connected(self):
        self.update(DEVICE, Connected=dbus.Boolean(True), ServicesResolved=dbus.Boolean(True))
        self.svc.seq += 1
        if self.w.get('audio'):
            self.svc.media.add_for(self)
        if self.w.get('gatt'):
            # BlueZ resolves the services a moment after the link is up, and only then says so.
            self.update(DEVICE, ServicesResolved=dbus.Boolean(False))

            def resolve():
                if self.p['Connected']:
                    self.svc.gatt.add_for(self)
                    self.update(DEVICE, ServicesResolved=dbus.Boolean(True))
                return False
            GLib.timeout_add(500, resolve)

    def link_lost(self, why):
        if self.p['Connected']:
            log('%s disconnected (%s)' % (self.address, why))
        self.svc.media.remove_for(self)
        self.svc.gatt.remove_for(self)
        self.update(DEVICE, Connected=dbus.Boolean(False), ServicesResolved=dbus.Boolean(False))

    def go_out_of_range(self):
        self.in_range = False
        self.invalidate(DEVICE, 'RSSI')
        if self.range_timer is not None:
            GLib.source_remove(self.range_timer)

        def back():
            self.range_timer = None
            self.in_range = True
            log('%s back in range' % self.address)
            return False
        self.range_timer = GLib.timeout_add_seconds(OUT_OF_RANGE_S, back)

    # -- pairing, shared by Pair() and an incoming pairing --

    def run_pairing(self, agent, incoming, done):
        """Asks `agent` whatever this device's pairing asks, then calls done(error_or_None)."""
        self.attempt += 1
        attempt = self.attempt
        kind = self.w['pairing']
        dev = dbus.ObjectPath(self.path)

        def finish(e):
            if attempt != self.attempt:
                return   # cancelled meanwhile; the canceller has answered already
            self.agent_waiting = None
            if e is None:
                self.update(DEVICE, Paired=dbus.Boolean(True), Bonded=dbus.Boolean(True))
            done(e)

        def agent_error(e):
            name = e.get_dbus_name() if hasattr(e, 'get_dbus_name') else ''
            log('  agent answered with error %s: %s' % (name, e))
            if name == 'org.bluez.Error.Rejected':
                return err('AuthenticationRejected', 'Authentication Rejected')
            if name == 'org.bluez.Error.Canceled':
                return err('AuthenticationCanceled', 'Authentication Canceled')
            if name == 'org.freedesktop.DBus.Error.NoReply':
                return err('AuthenticationTimeout', 'Authentication Timeout')
            return err('AuthenticationFailed', 'Authentication Failed')

        if kind == 'justworks' and not incoming:
            # BlueZ accepts a just-works pairing it started itself without asking anyone.
            GLib.timeout_add(1000, lambda: (finish(None), False)[1])
            return

        if agent is None:
            log('  no agent to ask: pairing %s fails' % self.address)
            GLib.timeout_add(500, lambda: (finish(err('AuthenticationFailed',
                                                      'Authentication Failed')), False)[1])
            return

        if kind == 'display':
            self.display_sequence(agent, lambda e: finish(e))
            return

        if kind == 'confirm':
            passkey = random.randint(0, 999999)
            method, sig, args = 'RequestConfirmation', 'ou', (dev, dbus.UInt32(passkey))
        elif kind == 'justworks':
            method, sig, args = 'RequestAuthorization', 'o', (dev,)
        else:  # pin
            method, sig, args = 'RequestPinCode', 'o', (dev,)

        def reply(*out):
            log('  agent %s -> %r' % (method, out))
            if method == 'RequestPinCode' and str(out[0] if out else '') != self.w.get('pin'):
                finish(err('AuthenticationFailed', 'Authentication Failed'))
            else:
                finish(None)

        self.agent_waiting = agent
        self.svc.call_agent(agent, method, sig, args, reply,
                            lambda e: finish(agent_error(e)))

    def display_sequence(self, agent, done):
        """DisplayPasskey with the count of typed digits climbing, as a keyboard pairing does."""
        passkey = dbus.UInt32(random.randint(0, 999999))
        dev = dbus.ObjectPath(self.path)
        step = [0]
        attempt = self.attempt

        def tick():
            if attempt != self.attempt:
                return False
            if step[0] > 6:
                done(None)
                return False
            self.svc.call_agent(agent, 'DisplayPasskey', 'ouq',
                                (dev, passkey, dbus.UInt16(step[0])),
                                lambda *a: None, lambda e: log('  DisplayPasskey error: %s' % e))
            step[0] += 1
            return True
        GLib.timeout_add(500, tick)

    # -- Device1 methods --

    @dbus.service.method(DEVICE, in_signature='', out_signature='', sender_keyword='sender',
                         async_callbacks=('ok', 'fail'))
    def Pair(self, sender=None, ok=None, fail=None):
        log('Pair %s by %s' % (self.address, sender))
        if self.busy == 'pair':
            return fail(err('InProgress', 'In Progress'))
        if self.p['Paired']:
            return fail(err('AlreadyExists', 'Already Exists'))
        if not self.svc.adapter.p['Powered']:
            return fail(err('NotReady', 'Resource Not Ready'))
        if not self.in_range:
            return GLib.timeout_add(3000, lambda: (fail(err('ConnectionAttemptFailed',
                                                            'Page Timeout')), False)[1])
        self.busy = 'pair'
        self.pending_pair = (ok, fail)

        def done(e):
            self.busy = None
            self.pending_pair = None
            log('Pair %s: %s' % (self.address, 'ok' if e is None else e.get_dbus_name()))
            if e is None:
                ok()
            else:
                fail(e)
        # The caller's own agent if it has one, else the default — as BlueZ's agent_get(sender).
        self.run_pairing(self.svc.agent_for(sender), False, done)

    @dbus.service.method(DEVICE, in_signature='', out_signature='')
    def CancelPairing(self):
        if self.pending_pair is None:
            raise err('DoesNotExist', 'Does Not Exist')
        log('CancelPairing %s' % self.address)
        ok, fail = self.pending_pair
        self.pending_pair = None
        self.busy = None
        self.attempt += 1       # orphan the outstanding agent call
        if self.agent_waiting is not None:
            self.svc.call_agent(self.agent_waiting, 'Cancel', '', (), lambda *a: None,
                                lambda e: None)
            self.agent_waiting = None
        fail(err('AuthenticationCanceled', 'Authentication Canceled'))

    @dbus.service.method(DEVICE, in_signature='', out_signature='', sender_keyword='sender',
                         async_callbacks=('ok', 'fail'))
    def Connect(self, sender=None, ok=None, fail=None):
        log('Connect %s by %s' % (self.address, sender))
        if self.busy is not None:
            return fail(err('InProgress', 'br-connection-busy'))
        if not self.svc.adapter.p['Powered']:
            return fail(err('NotReady', 'br-connection-adapter-not-powered'))
        if self.p['Connected']:
            return ok()   # BlueZ answers an already-connected device with success
        if not self.w.get('audio') and not self.w.get('gatt'):
            return GLib.timeout_add(800, lambda: (fail(err(
                'NotAvailable', 'br-connection-profile-unavailable')), False)[1])
        if not self.in_range:
            return GLib.timeout_add(3000, lambda: (fail(err(
                'Failed', 'br-connection-page-timeout')), False)[1])
        self.busy = 'connect'

        def up():
            self.busy = None
            self.cancel_forget()
            if not self.p['Paired']:
                # A2DP needs an authenticated link, so a just-works device bonds on the way up.
                self.update(DEVICE, Paired=dbus.Boolean(True), Bonded=dbus.Boolean(True))
            self.connected()
            log('Connect %s: ok' % self.address)
            ok()
            return False

        if self.p['Paired'] or self.w['pairing'] == 'justworks':
            GLib.timeout_add(1500, up)
            return

        # An unpaired device that needs a question pairs as part of the connection, as BlueZ
        # does when a profile needs security.
        def paired(e):
            self.busy = None
            if e is not None:
                fail(e)
            else:
                self.busy = 'connect'
                GLib.timeout_add(1000, up)
        self.run_pairing(self.svc.agent_for(sender), False, paired)

    @dbus.service.method(DEVICE, in_signature='', out_signature='', async_callbacks=('ok', 'fail'))
    def Disconnect(self, ok=None, fail=None):
        log('Disconnect %s' % self.address)
        if not self.p['Connected']:
            return ok()

        def down():
            self.link_lost('Disconnect()')
            ok()
            return False
        GLib.timeout_add(500, down)

    @dbus.service.method(DEVICE, in_signature='s', out_signature='', sender_keyword='sender',
                         async_callbacks=('ok', 'fail'))
    def ConnectProfile(self, uuid, sender=None, ok=None, fail=None):
        if str(uuid).lower() not in [u.lower() for u in self.w['uuids']] + [UUID_A2DP]:
            return fail(err('InvalidArguments', 'Invalid arguments in method call'))
        self.Connect(sender=sender, ok=ok, fail=fail)

    @dbus.service.method(DEVICE, in_signature='s', out_signature='', async_callbacks=('ok', 'fail'))
    def DisconnectProfile(self, uuid, ok=None, fail=None):
        self.Disconnect(ok=ok, fail=fail)




# ---- LE advertising and the GATT application manager, on the adapter -----------------------------

class BenchAdapter(Adapter):
    """Adapter1 plus what BlueZ hangs off the same object: LEAdvertisingManager1 (which reads each
    advertisement back from its client with GetAll, as src/advertising.c does) and GattManager1
    (which reads the application with GetManagedObjects, as src/gatt-database.c does)."""

    def __init__(self, svc, *a, **k):
        super().__init__(svc, *a, **k)
        self.advs = {}   # (sender, path) -> its properties
        self.adv_timers = {}
        self.apps = {}   # (sender, path) -> its managed objects

    def sync_adv_counts(self):
        n = len(self.advs)
        self.update(ADV_MGR, ActiveInstances=dbus.Byte(n),
                    SupportedInstances=dbus.Byte(max(0, ADV_INSTANCES - n)))

    @dbus.service.method(ADV_MGR, in_signature='oa{sv}', out_signature='', sender_keyword='sender',
                         async_callbacks=('ok', 'fail'))
    def RegisterAdvertisement(self, path, opts, sender=None, ok=None, fail=None):
        key = (sender, str(path))
        if key in self.advs:
            return fail(err('AlreadyExists', 'Already Exists'))
        if len(self.advs) >= ADV_INSTANCES:
            return fail(err('NotPermitted', 'Maximum advertisements reached'))
        if not self.p['Powered']:
            return fail(err('NotReady', 'Resource Not Ready'))

        def got(props):
            t = str(props.get('Type', ''))
            if t not in ('peripheral', 'broadcast'):
                log('RegisterAdvertisement %s: bad Type %r' % (path, t))
                return fail(err('Failed', 'Failed to parse advertisement.'))
            self.advs[key] = dict(props)
            self.sync_adv_counts()
            parts = ['%s=%s' % (k, describe(v)) for k, v in sorted(props.items())]
            log('RegisterAdvertisement %s by %s: %s' % (path, sender, ', '.join(parts)))
            timeout = int(props.get('Timeout', 0))
            if timeout:
                def expire():
                    self.adv_timers.pop(key, None)
                    if self.advs.pop(key, None) is not None:
                        log('advertisement %s timed out: Release' % path)
                        self.sync_adv_counts()
                        self.svc.conn.call_async(sender, path, ADV, 'Release', '', (),
                                                 lambda *a: None, lambda e: None)
                    return False
                self.adv_timers[key] = GLib.timeout_add_seconds(timeout, expire)
            ok()

        def bad(e):
            log('RegisterAdvertisement %s: GetAll failed: %s' % (path, e))
            fail(err('Failed', 'Failed to parse advertisement.'))
        self.svc.conn.call_async(sender, path, PROPS, 'GetAll', 's', (ADV,), got, bad)

    @dbus.service.method(ADV_MGR, in_signature='o', out_signature='', sender_keyword='sender')
    def UnregisterAdvertisement(self, path, sender=None):
        key = (sender, str(path))
        if self.advs.pop(key, None) is None:
            raise err('DoesNotExist', 'Does Not Exist')
        t = self.adv_timers.pop(key, None)
        if t is not None:
            GLib.source_remove(t)
        log('UnregisterAdvertisement %s' % path)
        self.sync_adv_counts()

    @dbus.service.method(GATT_MGR, in_signature='oa{sv}', out_signature='', sender_keyword='sender',
                         async_callbacks=('ok', 'fail'))
    def RegisterApplication(self, path, opts, sender=None, ok=None, fail=None):
        key = (sender, str(path))
        if key in self.apps:
            return fail(err('AlreadyExists', 'Already Exists'))

        def got(objs):
            svcs = [p for p, i in objs.items() if GATT_SVC in i]
            chrs = [p for p, i in objs.items() if GATT_CHR in i]
            dscs = [p for p, i in objs.items() if GATT_DSC in i]
            if not svcs:
                return fail(err('Failed', 'No valid service object found'))
            for p in chrs:
                c = objs[p][GATT_CHR]
                if 'UUID' not in c or 'Service' not in c or 'Flags' not in c:
                    log('RegisterApplication: %s lacks UUID/Service/Flags' % p)
                    return fail(err('Failed', 'Failed to create characteristic entry'))
            self.apps[key] = dict(objs)
            log('RegisterApplication %s by %s: %d services, %d characteristics, %d descriptors'
                % (path, sender, len(svcs), len(chrs), len(dscs)))
            for p in sorted(objs):
                for iface in (GATT_SVC, GATT_CHR, GATT_DSC):
                    if iface in objs[p]:
                        x = objs[p][iface]
                        log('  %s %s %s' % (p, x.get('UUID'), ','.join(x.get('Flags', []))))
            ok()

        def bad(e):
            log('RegisterApplication %s: GetManagedObjects failed: %s' % (path, e))
            fail(err('Failed', 'Failed to read the application'))
        self.svc.conn.call_async(sender, path, OBJMGR, 'GetManagedObjects', '', (), got, bad)

    @dbus.service.method(GATT_MGR, in_signature='o', out_signature='', sender_keyword='sender')
    def UnregisterApplication(self, path, sender=None):
        if self.apps.pop((sender, str(path)), None) is None:
            raise err('DoesNotExist', 'Does Not Exist')
        log('UnregisterApplication %s' % path)

    def client_gone(self, sender):
        super().client_gone(sender)
        for key in [k for k in self.advs if k[0] == sender]:
            self.advs.pop(key)
            t = self.adv_timers.pop(key, None)
            if t is not None:
                GLib.source_remove(t)
        self.sync_adv_counts()
        for key in [k for k in self.apps if k[0] == sender]:
            self.apps.pop(key)
            log('application %s gone with its client' % key[1])


def describe(v):
    if isinstance(v, dbus.Array) and v.signature == 'y':
        return hexs(v)
    if isinstance(v, dbus.Dictionary):
        return '{' + ', '.join('%s: %s' % (k, describe(x)) for k, x in v.items()) + '}'
    if isinstance(v, (list, dbus.Array)):
        return '[' + ', '.join(describe(x) for x in v) + ']'
    return str(v)


# ---- GATT client objects: the remote device's database ----------------------------------------------

class GattService(PropObject):
    def __init__(self, svc, path, uuid, dev, handle):
        super().__init__(svc, path, {GATT_SVC: {
            'UUID': dbus.String(uuid), 'Device': dbus.ObjectPath(dev.path),
            'Primary': dbus.Boolean(True), 'Includes': dbus.Array([], signature='o'),
            'Handle': dbus.UInt16(handle)}}, extra_ifaces=(INTROSPECT, PROPS))


class GattAttr(PropObject):
    """A characteristic or a descriptor. Reads and writes answer after a moment, as over the air;
    what the flags do not allow is refused with BlueZ's errors."""

    def __init__(self, svc, path, iface, props, flags, value, echo=False):
        props = dict(props)
        props['Flags'] = strv(flags)
        props['Value'] = ay(value)
        if iface == GATT_CHR:
            props['Notifying'] = dbus.Boolean(False)
            props['MTU'] = dbus.UInt16(247)
        super().__init__(svc, path, {iface: props}, extra_ifaces=(INTROSPECT, PROPS))
        self.iface = iface
        self.flags = flags
        self.echo = echo

    @property
    def p(self):
        return self.ifaces[self.iface]

    def read(self, opts, ok, fail):
        if 'read' not in self.flags and self.iface == GATT_CHR:
            return fail(err('NotPermitted', 'Read not permitted'))
        off = int(opts.get('offset', 0))
        v = list(self.p['Value'])
        if off > len(v):
            return fail(err('InvalidOffset', 'Invalid offset'))

        def done():
            # BlueZ caches what it read in Value, and says so.
            self.update(self.iface, Value=ay(v))
            ok(ay(v[off:]))
            return False
        GLib.timeout_add(50, done)

    def write(self, value, opts, ok, fail):
        t = str(opts.get('type', 'request'))
        if self.iface == GATT_CHR:
            allowed = 'write' in self.flags or ('write-without-response' in self.flags and t == 'command')
            if not allowed:
                return fail(err('NotPermitted', 'Write not permitted'))
        off = int(opts.get('offset', 0))
        v = list(self.p['Value'])
        if off > len(v):
            return fail(err('InvalidOffset', 'Invalid offset'))
        v = v[:off] + [int(b) for b in value]
        log('WriteValue %s (%s): %s' % (self.path, t, hexs(v)))

        def done():
            self.ifaces[self.iface]['Value'] = ay(v)
            # The echo characteristic notifies back what was written, as a peripheral's would.
            if self.echo and self.p.get('Notifying'):
                self.PropertiesChanged(self.iface, dbus.Dictionary({'Value': ay(v)}, signature='sv'),
                                       strv([]))
            ok()
            return False
        GLib.timeout_add(50, done)

    @dbus.service.method(GATT_CHR, in_signature='a{sv}', out_signature='ay', async_callbacks=('ok', 'fail'))
    def ReadValue(self, opts, ok=None, fail=None):
        self.read(opts, ok, fail)

    @dbus.service.method(GATT_CHR, in_signature='aya{sv}', out_signature='', async_callbacks=('ok', 'fail'))
    def WriteValue(self, value, opts, ok=None, fail=None):
        self.write(value, opts, ok, fail)

    @dbus.service.method(GATT_CHR, in_signature='', out_signature='')
    def StartNotify(self):
        if 'notify' not in self.flags and 'indicate' not in self.flags:
            raise err('NotSupported', 'Operation is not supported')
        log('StartNotify %s' % self.path)
        self.update(GATT_CHR, Notifying=dbus.Boolean(True))

    @dbus.service.method(GATT_CHR, in_signature='', out_signature='')
    def StopNotify(self):
        if not self.p.get('Notifying'):
            raise err('Failed', 'No notify session started')
        log('StopNotify %s' % self.path)
        self.update(GATT_CHR, Notifying=dbus.Boolean(False))


class GattDescriptor(GattAttr):
    @dbus.service.method(GATT_DSC, in_signature='a{sv}', out_signature='ay', async_callbacks=('ok', 'fail'))
    def ReadValue(self, opts, ok=None, fail=None):
        self.read(opts, ok, fail)

    @dbus.service.method(GATT_DSC, in_signature='aya{sv}', out_signature='', async_callbacks=('ok', 'fail'))
    def WriteValue(self, value, opts, ok=None, fail=None):
        self.write(value, opts, ok, fail)


class Gatt:
    """The GATT databases of the connected devices that have one (the Bench HRM). Exported when
    the device connects and its services resolve, removed when it disconnects."""

    def __init__(self, svc):
        self.svc = svc
        self.objs = {}   # device path -> [objects]
        self.hr = 72
        GLib.timeout_add_seconds(1, self.tick)

    def add_for(self, dev):
        if dev.path in self.objs:
            return
        s = self.svc
        d = dev.path
        objs = []

        def service(handle, uuid):
            o = GattService(s, '%s/service%04x' % (d, handle), uuid, dev, handle)
            objs.append(o)
            return o.path

        def chr_(spath, handle, uuid, flags, value, echo=False):
            o = GattAttr(s, '%s/char%04x' % (spath, handle), GATT_CHR,
                         {'UUID': dbus.String(uuid), 'Service': dbus.ObjectPath(spath),
                          'Handle': dbus.UInt16(handle)}, flags, value, echo)
            objs.append(o)
            return o.path

        def dsc(cpath, handle, uuid, flags, value):
            o = GattDescriptor(s, '%s/desc%04x' % (cpath, handle), GATT_DSC,
                               {'UUID': dbus.String(uuid), 'Characteristic': dbus.ObjectPath(cpath),
                                'Handle': dbus.UInt16(handle)}, flags, value)
            objs.append(o)

        hrs = service(0x000a, UUID_HRS)
        hrm = chr_(hrs, 0x000b, u16(0x2a37), ['notify'], [0x00, self.hr])
        dsc(hrm, 0x000d, u16(0x2902), ['read', 'write'], [0, 0])
        chr_(hrs, 0x000e, u16(0x2a38), ['read'], [0x01])
        bas = service(0x0010, UUID_BAS)
        bl = chr_(bas, 0x0011, u16(0x2a19), ['read', 'notify'], [90])
        dsc(bl, 0x0013, u16(0x2902), ['read', 'write'], [0, 0])
        dis = service(0x0014, UUID_DIS)
        chr_(dis, 0x0015, u16(0x2a29), ['read'], list(b'btbench fake'))
        chr_(dis, 0x0017, u16(0x2a24), ['read'], list(b'HRM-1'))
        chr_(dis, 0x0019, u16(0x2a26), ['read'], list(b'1.0.0'))
        echo = service(0x001b, UUID_ECHO_SVC)
        e = chr_(echo, 0x001c, UUID_ECHO_CHR, ['read', 'write', 'write-without-response', 'notify'],
                 list(b'echo'), echo=True)
        dsc(e, 0x001e, u16(0x2902), ['read', 'write'], [0, 0])
        dsc(e, 0x001f, u16(0x2901), ['read'], list(b'Echo: notifies what is written'))
        for o in objs:
            s.root.add(o)
        self.objs[d] = objs
        log('GATT of %s resolved: %d objects' % (dev.address, len(objs)))

    def remove_for(self, dev):
        for o in reversed(self.objs.pop(dev.path, [])):
            self.svc.root.remove(o)

    def tick(self):
        # The heart rate wanders, and is notified once a second to whoever subscribed; the battery
        # drains a percent every ten.
        self.hr = max(55, min(150, self.hr + random.randint(-3, 3)))
        for objs in self.objs.values():
            for o in objs:
                if not isinstance(o, GattAttr) or o.iface != GATT_CHR or not o.p.get('Notifying'):
                    continue
                uuid = str(o.p['UUID'])
                if uuid == u16(0x2a37):
                    o.update(GATT_CHR, Value=ay([0x00, self.hr]))
                elif uuid == u16(0x2a19) and int(time.time()) % 10 == 0:
                    o.update(GATT_CHR, Value=ay([max(0, int(o.p['Value'][0]) - 1)]))
        return True


# ---- Media: remote stream endpoints, transports, AVRCP players -------------------------------------

SBC_CAPS = [0xff, 0xff, 0x02, 0x35]           # every rate/mode/block/subband, bitpool 2..53
AAC_CAPS = [0xc0, 0xff, 0xfc, 0x84, 0xe2, 0x00]  # MPEG-2/4 LC, 8..96 kHz, 1-2 ch, VBR, 320 kbit/s


def sbc_config(rate):
    # Joint stereo, 16 blocks, 8 subbands, loudness, bitpool 2..53.
    return [{48000: 0x10, 44100: 0x20, 32000: 0x40, 16000: 0x80}[rate] | 0x01, 0x15, 0x02, 0x35]


class Transport(PropObject):
    def __init__(self, svc, path, dev, uuid, endpoint, config, state, delay):
        p = {'Device': dbus.ObjectPath(dev.path), 'UUID': dbus.String(uuid), 'Codec': dbus.Byte(0),
             'Configuration': ay(config), 'State': dbus.String(state), 'Endpoint': dbus.ObjectPath(endpoint),
             'Volume': dbus.UInt16(100)}
        if delay is not None:
            p['Delay'] = dbus.UInt16(delay)
        w = {TRANSPORT + '.Volume': self.set_volume}
        super().__init__(svc, path, {TRANSPORT: p}, writable=w, extra_ifaces=(INTROSPECT, PROPS))

    def set_volume(self, v):
        if int(v) > 127:
            raise dbus.DBusException('Invalid arguments in method call',
                                     name='org.freedesktop.DBus.Error.InvalidArgs')
        self.update(TRANSPORT, Volume=dbus.UInt16(int(v)))

    @dbus.service.method(TRANSPORT, in_signature='', out_signature='hqq')
    def Acquire(self):
        raise err('NotAuthorized', 'Operation Not Authorized')

    @dbus.service.method(TRANSPORT, in_signature='', out_signature='hqq')
    def TryAcquire(self):
        raise err('NotAvailable', 'Operation currently not available')

    @dbus.service.method(TRANSPORT, in_signature='', out_signature='')
    def Release(self):
        pass


# What a phone's AVRCP target looks like through BlueZ (org.bluez.MediaPlayer1, with the browsing
# of MediaFolder1/MediaItem1): a playlist it plays through, with the methods a controller calls.
# The position advances while playing and is published every 5 s, about as often as a phone sends
# PLAYBACK_POS_CHANGED; between those, a console interpolates.
TRACKS = [
    ('Test Tone 1 kHz', 'btbench', 'Fakes', 'Test signals', 180000),
    ('Pink Noise', 'btbench', 'Fakes', 'Test signals', 240000),
    ('Silence (for the noise floor)', 'btbench', 'Fakes', 'Test signals', 60000),
    ('Sweep 20 Hz - 20 kHz', 'btbench', 'Fakes', 'Test signals', 30000),
]
REPEAT = ('off', 'singletrack', 'alltracks', 'group')
SHUFFLE = ('off', 'alltracks', 'group')


class Player(PropObject):
    def __init__(self, svc, path, dev):
        self.index = 0
        self.position = 42000
        self.status = 'playing'
        self.clock_at = time.monotonic()
        p = {'Name': dbus.String('Music'), 'Type': dbus.String('Audio'), 'Subtype': dbus.String('Audio Book'),
             'Status': dbus.String('playing'), 'Position': dbus.UInt32(42000),
             'Device': dbus.ObjectPath(dev.path), 'Track': self.track(),
             'Repeat': dbus.String('off'), 'Shuffle': dbus.String('off'),
             'Browsable': dbus.Boolean(True), 'Searchable': dbus.Boolean(False),
             'Playlist': dbus.ObjectPath(path + '/NowPlaying')}
        w = {PLAYER + '.Repeat': lambda v: self.set_mode('Repeat', v, REPEAT),
             PLAYER + '.Shuffle': lambda v: self.set_mode('Shuffle', v, SHUFFLE)}
        super().__init__(svc, path, {PLAYER: p, FOLDER: {'Name': dbus.String('/NowPlaying'),
                                                          'NumberOfItems': dbus.UInt32(len(TRACKS))}},
                         writable=w, extra_ifaces=(INTROSPECT, PROPS))
        self.items = []
        for i, t in enumerate(TRACKS):
            self.items.append(Item(svc, '%s/NowPlaying/item%d' % (path, i + 1), self, i))
        self.timer = GLib.timeout_add_seconds(5, self.publish_position)

    def track(self):
        t = TRACKS[self.index]
        return dbus.Dictionary({'Title': dbus.String(t[0]), 'Artist': dbus.String(t[1]),
                                'Album': dbus.String(t[2]), 'Genre': dbus.String(t[3]),
                                'Duration': dbus.UInt32(t[4]), 'TrackNumber': dbus.UInt32(self.index + 1),
                                'NumberOfTracks': dbus.UInt32(len(TRACKS))}, signature='sv')

    def stop_clock(self):
        if self.timer:
            GLib.source_remove(self.timer)
            self.timer = None

    def now_position(self):
        if self.status == 'playing':
            return self.position + int((time.monotonic() - self.clock_at) * 1000)
        if self.status in ('forward-seek', 'reverse-seek'):
            step = 4 if self.status == 'forward-seek' else -4
            return max(0, self.position + int((time.monotonic() - self.clock_at) * 1000 * step))
        return self.position

    def settle(self):
        self.position = min(self.now_position(), TRACKS[self.index][4])
        self.clock_at = time.monotonic()

    def publish_position(self):
        self.settle()
        if self.position >= TRACKS[self.index][4] and self.status == 'playing':
            self.go(1 if self.ifaces[PLAYER]['Repeat'] != 'singletrack' else 0)
        self.update(PLAYER, Position=dbus.UInt32(self.position))
        return True

    def set_status(self, status):
        self.settle()
        self.status = status
        log('player %s: %s' % (self.path, status))
        self.update(PLAYER, Status=dbus.String(status), Position=dbus.UInt32(self.position))

    def go(self, step):
        self.index = (self.index + step) % len(TRACKS)
        self.position = 0
        self.clock_at = time.monotonic()
        log('player %s: track %d %s' % (self.path, self.index + 1, TRACKS[self.index][0]))
        self.update(PLAYER, Track=self.track(), Position=dbus.UInt32(0))

    def set_mode(self, name, v, allowed):
        if str(v) not in allowed:
            raise dbus.DBusException('Invalid arguments in method call',
                                     name='org.freedesktop.DBus.Error.InvalidArgs')
        self.update(PLAYER, **{name: dbus.String(str(v))})

    @dbus.service.method(PLAYER, in_signature='', out_signature='')
    def Play(self):
        self.set_status('playing')

    @dbus.service.method(PLAYER, in_signature='', out_signature='')
    def Pause(self):
        self.set_status('paused')

    @dbus.service.method(PLAYER, in_signature='', out_signature='')
    def Stop(self):
        self.set_status('stopped')
        self.position = 0
        self.update(PLAYER, Position=dbus.UInt32(0))

    @dbus.service.method(PLAYER, in_signature='', out_signature='')
    def Next(self):
        self.go(1)

    @dbus.service.method(PLAYER, in_signature='', out_signature='')
    def Previous(self):
        # As phones do: back to the start of the track unless it has only just begun.
        self.settle()
        if self.position > 3000:
            self.position = 0
            self.update(PLAYER, Position=dbus.UInt32(0))
        else:
            self.go(-1)

    @dbus.service.method(PLAYER, in_signature='', out_signature='')
    def FastForward(self):
        self.set_status('forward-seek')

    @dbus.service.method(PLAYER, in_signature='', out_signature='')
    def Rewind(self):
        self.set_status('reverse-seek')

    @dbus.service.method(PLAYER, in_signature='y', out_signature='')
    def Press(self, key):
        # The AV/C operation ids of the keys above (Play 0x44, Stop 0x45, Pause 0x46, ...).
        keys = {0x44: self.Play, 0x45: self.Stop, 0x46: self.Pause, 0x4b: self.Next, 0x4c: self.Previous}
        log('player %s: Press 0x%02x' % (self.path, int(key)))
        if int(key) in keys:
            keys[int(key)]()
        elif int(key) not in (0x41, 0x42):  # volume up/down: the transport's business
            raise err('NotSupported', 'Operation is not supported')

    @dbus.service.method(PLAYER, in_signature='y', out_signature='')
    def Hold(self, key):
        log('player %s: Hold 0x%02x' % (self.path, int(key)))

    @dbus.service.method(PLAYER, in_signature='', out_signature='')
    def Release(self):
        log('player %s: Release' % self.path)
        if self.status in ('forward-seek', 'reverse-seek'):
            self.set_status('playing')

    @dbus.service.method(FOLDER, in_signature='a{sv}', out_signature='a{oa{sv}}')
    def ListItems(self, flt):
        return dbus.Dictionary({dbus.ObjectPath(i.path): dbus.Dictionary(i.ifaces[ITEM], signature='sv')
                                for i in self.items}, signature='oa{sv}')

    @dbus.service.method(FOLDER, in_signature='o', out_signature='')
    def ChangeFolder(self, folder):
        if not str(folder).endswith('/NowPlaying'):
            raise err('InvalidArguments', 'Invalid arguments in method call')


class Item(PropObject):
    def __init__(self, svc, path, player, index):
        self.player = player
        self.index = index
        t = TRACKS[index]
        super().__init__(svc, path, {ITEM: {
            'Player': dbus.ObjectPath(player.path), 'Name': dbus.String(t[0]), 'Type': dbus.String('audio'),
            'Playable': dbus.Boolean(True),
            'Metadata': dbus.Dictionary({'Title': dbus.String(t[0]), 'Artist': dbus.String(t[1]),
                                         'Album': dbus.String(t[2]), 'Duration': dbus.UInt32(t[4])},
                                        signature='sv')}}, extra_ifaces=(INTROSPECT, PROPS))

    @dbus.service.method(ITEM, in_signature='', out_signature='')
    def Play(self):
        self.player.go(self.index - self.player.index)
        self.player.set_status('playing')


class Media:
    """What an A2DP connection brings: the device's stream endpoints, a configured transport, and
    for a phone its AVRCP player. No audio behind any of it: Acquire is refused."""

    def __init__(self, svc):
        self.svc = svc
        self.objs = {}   # device path -> [objects]

    def add_for(self, dev):
        if dev.path in self.objs:
            return
        s = self.svc
        w = dev.w
        objs = []
        playback = w['audio'] == 'playback'
        # The device's own role is the endpoint's UUID; ours is the transport's.
        remote_role = UUID_AUDIO_SINK if playback else UUID_AUDIO_SOURCE
        local_role = UUID_AUDIO_SOURCE if playback else UUID_AUDIO_SINK
        sep1 = dev.path + '/sep1'
        objs.append(PropObject(s, sep1, {ENDPOINT: {
            'UUID': dbus.String(remote_role), 'Codec': dbus.Byte(0), 'Capabilities': ay(SBC_CAPS),
            'Device': dbus.ObjectPath(dev.path), 'DelayReporting': dbus.Boolean(playback)}},
            extra_ifaces=(INTROSPECT, PROPS)))
        if playback:
            objs.append(PropObject(s, dev.path + '/sep2', {ENDPOINT: {
                'UUID': dbus.String(remote_role), 'Codec': dbus.Byte(2), 'Capabilities': ay(AAC_CAPS),
                'Device': dbus.ObjectPath(dev.path), 'DelayReporting': dbus.Boolean(True)}},
                extra_ifaces=(INTROSPECT, PROPS)))
        t = Transport(s, sep1 + '/fd0', dev, local_role, sep1, sbc_config(w.get('rate', 48000)),
                      'idle' if playback else 'pending', 1500 if playback else None)
        objs.append(t)
        if not playback:
            player = Player(s, dev.path + '/player0', dev)
            objs.append(player)
            objs.extend(player.items)
        for o in objs:
            s.root.add(o)
        self.objs[dev.path] = objs
        log('media: %s endpoints and a %s transport for %s' % (
            len([o for o in objs if ENDPOINT in o.ifaces]), 'source' if playback else 'sink', dev.address))
        if playback:
            # A speaker revises its reported delay once it has settled; let the console see one.
            GLib.timeout_add_seconds(5, lambda: (t.path in s.root.objects and
                                                 t.update(TRANSPORT, Delay=dbus.UInt16(1800)), False)[1])
        else:
            # A phone opens its stream when it starts to play, a moment after the link is up.
            GLib.timeout_add(2000, lambda: (t.path in s.root.objects and
                                            t.update(TRANSPORT, State=dbus.String('active')), False)[1])

    def remove_for(self, dev):
        for o in reversed(self.objs.pop(dev.path, [])):
            if isinstance(o, Player):
                o.stop_clock()
            self.svc.root.remove(o)

    def set_state(self, dev, state):
        for o in self.objs.get(dev.path, []):
            if TRANSPORT in o.ifaces:
                o.update(TRANSPORT, State=dbus.String(state))
                return True
        return False


# ---- test hooks ---------------------------------------------------------------------------------

class Fake(dbus.service.Object):
    """org.btbench.Fake1: what the other side of a radio link would do."""

    def __init__(self, svc):
        super().__init__(svc.conn, '/org/btbench/fake')
        self.svc = svc
        self.watch = {}  # app char path -> signal match, for AppSubscribe

    def device(self, address, create=True):
        address = str(address).upper()
        d = self.svc.devices.get(address)
        if d is None and create:
            w = world_entry(address)
            if w is None:
                raise dbus.DBusException('No such device in the fake world: ' + address,
                                         name='org.btbench.Error.NoSuchDevice')
            d = self.svc.add_device(w)
        if d is None:
            raise dbus.DBusException('Device not present: ' + address,
                                     name='org.btbench.Error.NoSuchDevice')
        return d

    @dbus.service.method(FAKE, in_signature='s', out_signature='s',
                         async_callbacks=('ok', 'fail'))
    def IncomingPair(self, address, ok=None, fail=None):
        """That device pairs with us, then (unless trusted) asks to use A2DP, then connects."""
        d = self.device(address)
        log('IncomingPair %s' % d.address)
        a = self.svc.adapter.p
        if not a['Powered']:
            return ok('rejected: adapter is off')
        if d.p['Paired']:
            return ok('already paired')
        if not a['Pairable']:
            log('  not pairable: rejected without asking the agent')
            return ok('rejected: not pairable')
        agent = self.svc.agent_for(None)

        def paired(e):
            if e is not None:
                return ok('pairing failed: ' + e.get_dbus_name())
            if d.p['Trusted']:
                d.connected()
                return ok('paired+connected')

            def authorized(*_):
                log('  agent AuthorizeService -> ok')
                d.connected()
                ok('paired+authorized+connected')

            def refused(e):
                log('  agent AuthorizeService -> %s' % e.get_dbus_name())
                ok('paired, service refused: ' + e.get_dbus_name())
            if agent is None:
                return ok('paired, service refused: no agent')
            self.svc.call_agent(agent, 'AuthorizeService', 'os',
                                (dbus.ObjectPath(d.path), dbus.String(UUID_A2DP)),
                                authorized, refused)
        d.run_pairing(agent, True, paired)

    @dbus.service.method(FAKE, in_signature='s', out_signature='s',
                         async_callbacks=('ok', 'fail'))
    def Display(self, address, ok=None, fail=None):
        d = self.device(address)
        agent = self.svc.agent_for(None)
        if agent is None:
            return ok('no agent')
        log('Display %s' % d.address)
        d.attempt += 1

        def done(e):
            d.update(DEVICE, Paired=dbus.Boolean(True), Bonded=dbus.Boolean(True))
            ok('paired')
        d.display_sequence(agent, done)

    @dbus.service.method(FAKE, in_signature='s', out_signature='')
    def DropLink(self, address):
        d = self.device(address, create=False)
        d.link_lost('DropLink: out of range')
        d.go_out_of_range()

    @dbus.service.method(FAKE, in_signature='s', out_signature='')
    def StartStream(self, address):
        d = self.device(address, create=False)
        if not d.p['Connected'] or not self.svc.media.set_state(d, 'active'):
            raise dbus.DBusException('No audio link', name='org.btbench.Error.NotConnected')

    @dbus.service.method(FAKE, in_signature='s', out_signature='')
    def StopStream(self, address):
        d = self.device(address, create=False)
        if not self.svc.media.set_state(d, 'idle'):
            raise dbus.DBusException('No audio link', name='org.btbench.Error.NotConnected')

    @dbus.service.method(FAKE, in_signature='', out_signature='s')
    def Advertisements(self):
        a = self.svc.adapter
        out = ['%s %s: %s' % (k[0], k[1], ', '.join('%s=%s' % (n, describe(v)) for n, v in sorted(p.items())))
               for k, p in a.advs.items()]
        return '\n'.join(out) or '(none)'

    def app_char(self, uuid):
        uuid = str(uuid).lower()
        if len(uuid) == 4:
            uuid = u16(int(uuid, 16))
        for (sender, _), objs in self.svc.adapter.apps.items():
            for p, ifaces in objs.items():
                if GATT_CHR in ifaces and str(ifaces[GATT_CHR].get('UUID', '')).lower() == uuid:
                    return sender, str(p)
        raise dbus.DBusException('No registered application has characteristic ' + uuid,
                                 name='org.btbench.Error.NoSuchCharacteristic')

    # A remote device using the bench's own GATT server: read, write and subscribe to one of the
    # registered application's characteristics, as BlueZ would on that device's behalf.
    @dbus.service.method(FAKE, in_signature='s', out_signature='s', async_callbacks=('ok', 'fail'))
    def AppRead(self, uuid, ok=None, fail=None):
        sender, path = self.app_char(uuid)
        opts = dbus.Dictionary({'device': dbus.ObjectPath(dev_path('C0:FF:EE:00:00:01')),
                                'offset': dbus.UInt16(0)}, signature='sv')
        self.svc.conn.call_async(sender, path, GATT_CHR, 'ReadValue', 'a{sv}', (opts,),
                                 lambda v: ok(hexs(v)), fail)

    @dbus.service.method(FAKE, in_signature='ss', out_signature='', async_callbacks=('ok', 'fail'))
    def AppWrite(self, uuid, value, ok=None, fail=None):
        sender, path = self.app_char(uuid)
        opts = dbus.Dictionary({'device': dbus.ObjectPath(dev_path('C0:FF:EE:00:00:01')),
                                'type': dbus.String('request')}, signature='sv')
        self.svc.conn.call_async(sender, path, GATT_CHR, 'WriteValue', 'aya{sv}',
                                 (ay(bytes.fromhex(str(value))), opts), lambda *a: ok(), fail)

    @dbus.service.method(FAKE, in_signature='s', out_signature='', async_callbacks=('ok', 'fail'))
    def AppSubscribe(self, uuid, ok=None, fail=None):
        sender, path = self.app_char(uuid)

        def changed(iface, props, _inv):
            if 'Value' in props:
                log('notification from %s: %s' % (path, hexs(props['Value'])))
        if path not in self.watch:
            self.watch[path] = self.svc.conn.add_signal_receiver(changed, 'PropertiesChanged', PROPS,
                                                                 sender, path)
        self.svc.conn.call_async(sender, path, GATT_CHR, 'StartNotify', '', (), lambda *a: ok(), fail)

    @dbus.service.method(FAKE, in_signature='', out_signature='')
    def Reset(self):
        log('Reset')
        self.svc.reset()


# ---- the service --------------------------------------------------------------------------------

class Service:
    def __init__(self, conn):
        self.conn = conn
        self.agents = {}          # unique name -> (path, capability)
        self.default_agent = None
        self.devices = {}         # address -> Device
        self.seq = 0
        self.root = ObjectManager(self, '/')
        self.agent_mgr = AgentManager(self)
        self.adapter = BenchAdapter(self)
        if ADAPTER_PATH != DESK_ADAPTER[0]:
            self.root.add(Adapter(self, DESK_ADAPTER[0], DESK_ADAPTER[1], world=False))
        self.media = Media(self)
        self.gatt = Gatt(self)
        self.fake = Fake(self)
        self.root.add(self.agent_mgr)
        self.root.add(self.adapter)
        self.populate()
        # A client that goes away takes its agent, its scan and its registrations with it.
        conn.add_signal_receiver(self.owner_changed, 'NameOwnerChanged', 'org.freedesktop.DBus',
                                 'org.freedesktop.DBus', '/org/freedesktop/DBus')

    def populate(self):
        for w in WORLD:
            if w.get('known'):
                self.add_device(w)

    def reset(self):
        for d in list(self.devices.values()):
            self.remove_device(d)
        a = self.adapter
        for sender in list(a.discovery):
            a.discovery.pop(sender)
        a.filters.clear()
        a.sync_discovering()
        a.cancel_timer('disc_timer')
        a.cancel_timer('pair_timer')
        a.update(ADAPTER, Powered=dbus.Boolean(True), PowerState=dbus.String('on'),
                 Discoverable=dbus.Boolean(False), DiscoverableTimeout=dbus.UInt32(180),
                 Pairable=dbus.Boolean(True), PairableTimeout=dbus.UInt32(0),
                 Alias=dbus.String('BlueZ 5.84'))
        self.populate()

    def add_device(self, w):
        d = Device(self, w)
        self.devices[w['address']] = d
        self.root.add(d)
        log('device %s (%s) appeared' % (w['address'], w['name']))
        return d

    def remove_device(self, d):
        d.attempt += 1
        d.cancel_forget()
        self.media.remove_for(d)
        self.gatt.remove_for(d)
        self.devices.pop(d.address, None)
        self.root.remove(d)

    def agent_for(self, sender):
        if sender in self.agents:
            return (sender,) + self.agents[sender]
        if self.default_agent in self.agents:
            return (self.default_agent,) + self.agents[self.default_agent]
        return None

    def call_agent(self, agent, method, sig, args, reply, error):
        sender, path, _cap = agent
        log('-> agent %s %s%r' % (sender, method, tuple(str(a) for a in args)))
        self.conn.call_async(sender, path, AGENT, method, sig, args, reply, error,
                             timeout=AGENT_TIMEOUT_S)

    def drop_agent(self, sender):
        if self.agents.pop(sender, None) is not None:
            log('agent of %s unregistered' % sender)
        if self.default_agent == sender:
            self.default_agent = None

    def owner_changed(self, name, old, new):
        if name.startswith(':') and not new:
            self.drop_agent(name)
            self.adapter.client_gone(name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--address', required=True, help='the private bus to serve on')
    ap.add_argument('--adapters', type=int, choices=(1, 2), default=1,
                    help='2: the world on hci1, and an adapter with nothing in range on hci0')
    args = ap.parse_args()
    if args.adapters == 2:
        global ADAPTER_PATH
        ADAPTER_PATH = '/org/bluez/hci1'

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    conn = dbus.bus.BusConnection(args.address)
    # Gone with its bus: a fake outliving the bus it served would only confuse the next run.
    conn.set_exit_on_disconnect(True)
    svc = Service(conn)
    name = dbus.service.BusName('org.bluez', conn, do_not_queue=True)
    log('fake BlueZ on %s' % args.address)
    loop = GLib.MainLoop()
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    del name, svc


if __name__ == '__main__':
    sys.exit(main())
