"""A fake obexd (org.bluez.obex) for the fake BlueZ's private bus.

btbenchd's OBEX tester drives obexd's client API (Client1, Session1, ObjectPush1, FileTransfer1,
PhonebookAccess1, MessageAccess1/Message1, Image1, Transfer1) and registers an obex Agent1 for
incoming pushes. This plays obexd for the scripted world: sessions to the devices that offer
the profile, a small remote file tree, a phonebook, a message store and AVRCP cover art, with
transfers that progress and complete (writing real files where the client asked), and an incoming
push (ctl obex-push) that asks the registered agent where to put it.

It runs on a connection of its own: obexd's ObjectManager is at "/", where fake_bluez.py's is too.
"""

import os
import struct
import time
import zlib

import dbus
import dbus.service
from gi.repository import GLib

OBEX = 'org.bluez.obex'
PROPS = 'org.freedesktop.DBus.Properties'
OBJMGR = 'org.freedesktop.DBus.ObjectManager'
CLIENT = 'org.bluez.obex.Client1'
AGENT_MGR = 'org.bluez.obex.AgentManager1'
SESSION = 'org.bluez.obex.Session1'
TRANSFER = 'org.bluez.obex.Transfer1'
OPP = 'org.bluez.obex.ObjectPush1'
FTP = 'org.bluez.obex.FileTransfer1'
PBAP = 'org.bluez.obex.PhonebookAccess1'
MAP = 'org.bluez.obex.MessageAccess1'
MESSAGE = 'org.bluez.obex.Message1'
IMAGE = 'org.bluez.obex.Image1'

TARGETS = {
    'opp': ('00001105-0000-1000-8000-00805f9b34fb', OPP),
    'ftp': ('f9ec7bc4-953c-11d2-984e-525400dc9e09', FTP),
    'pbap': ('796135f0-f0c5-11d8-0966-0800200c9a66', PBAP),
    'map': ('bb582b40-420c-11db-b0de-0800200c9a66', MAP),
    'bip-avrcp': ('7163dd54-4a7e-11e2-b47c-0050c2490048', IMAGE),
}

# Who offers what. The phone has everything; the speakers take a pushed file and nothing else.
PROFILES = {
    '5C:E9:1E:22:40:01': ('opp', 'ftp', 'pbap', 'map', 'bip-avrcp'),
    'A0:B1:C2:D3:E4:F5': ('opp',),
    'F8:DF:15:0A:11:3C': ('opp',),
}

# The phone's file tree (FTP) and stores (PBAP, MAP).
TREE = {
    '/': {'DCIM': None, 'Music': None, 'notes.txt': b'shopping: milk, bread\n'},
    '/DCIM': {'IMG_0001.jpg': b'\xff\xd8\xff\xe0' + b'\0' * 2044},
    '/Music': {'tone.wav': b'RIFF' + b'\0' * 4092},
}
CONTACTS = [('0.vcf', 'Pixel Owner', '+421900000000'), ('1.vcf', 'Alice Bench', '+421900111111'),
            ('2.vcf', 'Bob Tester', '+421900222222')]
MESSAGES = {
    'inbox': [('Alice Bench', '+421900111111', 'Lunch?', 'Are we still on for 12:30?', False),
              ('Bob Tester', '+421900222222', 'Build', 'The image built, flash it', True)],
    'sent': [('Pixel Owner', '+421900000000', 'Re: Build', 'Flashing now', True)],
}
FOLDER_MAP = ['inbox', 'outbox', 'sent', 'deleted']


def log(*a):
    print(time.strftime('%H:%M:%S'), 'obex:', *a, flush=True)


def err(name, msg):
    return dbus.DBusException(msg, name='org.bluez.obex.Error.' + name)


def vcard(name, tel):
    return ('BEGIN:VCARD\r\nVERSION:3.0\r\nFN:%s\r\nN:%s;;;;\r\nTEL;TYPE=CELL:%s\r\nEND:VCARD\r\n'
            % (name, ';'.join(reversed(name.split(' ', 1))), tel)).encode()


def png(w, h, rgb):
    """A plain one-colour PNG with a lighter diagonal, so the console has an image to show."""
    rows = b''
    for y in range(h):
        row = b'\0'
        for x in range(w):
            c = rgb if abs(x - y) > 6 else tuple(min(255, v + 90) for v in rgb)
            row += bytes(c)
        rows += row

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


class Obj(dbus.service.Object):
    """An exported object with properties, announced through the obex ObjectManager."""

    def __init__(self, svc, path, ifaces):
        super().__init__(svc.conn, path)
        self.svc = svc
        self.path = path
        self.ifaces = ifaces

    def update(self, iface, **kw):
        self.ifaces[iface].update(kw)
        self.PropertiesChanged(iface, dbus.Dictionary(kw, signature='sv'), dbus.Array([], signature='s'))

    @dbus.service.method(PROPS, in_signature='ss', out_signature='v')
    def Get(self, iface, name):
        return self.ifaces[iface][name]

    @dbus.service.method(PROPS, in_signature='s', out_signature='a{sv}')
    def GetAll(self, iface):
        return dbus.Dictionary(self.ifaces.get(iface, {}), signature='sv')

    @dbus.service.signal(PROPS, signature='sa{sv}as')
    def PropertiesChanged(self, iface, changed, invalidated):
        pass


class Transfer(Obj):
    """Queued, then active for about a second (Transferred climbing), then complete: the file is
    written where the client asked (or where the agent said, for a push), and obexd removes the
    object soon after, as the real one does."""

    def __init__(self, svc, path, session, name, data, filename, on_done=None):
        super().__init__(svc, path, {TRANSFER: {
            'Status': dbus.String('queued'), 'Name': dbus.String(name), 'Size': dbus.UInt64(len(data)),
            'Transferred': dbus.UInt64(0), 'Filename': dbus.String(filename or ''),
            'Session': dbus.ObjectPath(session)}})
        self.data = data
        self.on_done = on_done
        self.steps = 0
        svc.add(self)
        GLib.timeout_add(150, self.step)

    def step(self):
        if self.ifaces[TRANSFER]['Status'] == 'suspended':
            return True
        self.steps += 1
        if self.steps < 6:
            self.update(TRANSFER, Status=dbus.String('active'),
                        Transferred=dbus.UInt64(len(self.data) * self.steps // 6))
            return True
        fn = str(self.ifaces[TRANSFER]['Filename'])
        if fn and self.data is not None:
            with open(fn, 'wb') as f:
                f.write(self.data)
        self.update(TRANSFER, Status=dbus.String('complete'), Transferred=dbus.UInt64(len(self.data)))
        log('transfer %s complete: %s (%d bytes)%s' % (self.path, self.ifaces[TRANSFER]['Name'], len(self.data),
                                                       ' -> ' + fn if fn else ''))
        if self.on_done:
            self.on_done()
        GLib.timeout_add(1000, lambda: (self.svc.remove(self), False)[1])
        return False

    @dbus.service.method(TRANSFER)
    def Cancel(self):
        self.update(TRANSFER, Status=dbus.String('error'))
        self.steps = 99
        GLib.timeout_add(500, lambda: (self.svc.remove(self), False)[1])

    @dbus.service.method(TRANSFER)
    def Suspend(self):
        self.update(TRANSFER, Status=dbus.String('suspended'))

    @dbus.service.method(TRANSFER)
    def Resume(self):
        self.update(TRANSFER, Status=dbus.String('active'))


class Session(Obj):
    def __init__(self, svc, n, dest, target, opts):
        uuid, self.profile = TARGETS[target]
        path = '/org/bluez/obex/client/session%d' % n
        props = {'Source': dbus.String(svc.adapter_address), 'Destination': dbus.String(dest),
                 'Target': dbus.String(uuid), 'Root': dbus.String('')}
        if 'PSM' in opts:
            props['PSM'] = dbus.UInt16(int(opts['PSM']))
        elif 'Channel' in opts:
            props['Channel'] = dbus.Byte(int(opts['Channel']))
        super().__init__(svc, path, {SESSION: props, self.profile: {}})
        self.target = target
        self.cwd = '/'
        self.pb = 'pb'
        self.map_folder = ''
        self.ntransfer = 0
        self.messages = []
        svc.add(self)

    def transfer(self, name, data, filename):
        self.ntransfer += 1
        if not filename:
            filename = '/tmp/obex-fake-%d-%d' % (os.getpid(), self.ntransfer)
        t = Transfer(self.svc, '%s/transfer%d' % (self.path, self.ntransfer), self.path, name, data, filename)
        return dbus.ObjectPath(t.path), dbus.Dictionary(t.ifaces[TRANSFER], signature='sv')

    def need(self, iface):
        if self.profile != iface:
            raise err('NotSupported', 'Not supported by this session')

    # ---- ObjectPush1 ----
    @dbus.service.method(OPP, in_signature='s', out_signature='oa{sv}')
    def SendFile(self, src):
        self.need(OPP)
        if not os.path.isfile(src):
            raise err('InvalidArguments', 'Unable to open file')
        data = open(src, 'rb').read()
        log('%s receives %s (%d bytes)' % (self.ifaces[SESSION]['Destination'], os.path.basename(src), len(data)))
        p, props = self.transfer(os.path.basename(src), data, src)
        props = dict(props)
        return p, dbus.Dictionary(props, signature='sv')

    @dbus.service.method(OPP, in_signature='s', out_signature='oa{sv}')
    def PullBusinessCard(self, target):
        self.need(OPP)
        return self.transfer('', vcard(*CONTACTS[0][1:]), target)

    @dbus.service.method(OPP, in_signature='ss', out_signature='oa{sv}')
    def ExchangeBusinessCards(self, client, target):
        self.need(OPP)
        return self.transfer('', vcard(*CONTACTS[0][1:]), target)

    # ---- FileTransfer1 ----
    def node(self, path):
        return TREE.setdefault(path, {})

    @dbus.service.method(FTP, in_signature='s')
    def ChangeFolder(self, folder):
        self.need(FTP)
        if folder in ('', '/'):
            self.cwd = '/'
            return
        if folder == '..':
            self.cwd = self.cwd.rsplit('/', 1)[0] or '/'
            return
        new = (self.cwd.rstrip('/') + '/' + folder)
        if folder not in TREE.get(self.cwd, {}) or TREE[self.cwd][folder] is not None:
            raise err('Failed', 'Not Found')
        self.cwd = new

    @dbus.service.method(FTP, in_signature='s')
    def CreateFolder(self, folder):
        self.need(FTP)
        TREE[self.cwd][folder] = None
        TREE[(self.cwd.rstrip('/') + '/' + folder)] = {}
        self.cwd = self.cwd.rstrip('/') + '/' + folder

    @dbus.service.method(FTP, in_signature='', out_signature='aa{sv}')
    def ListFolder(self):
        self.need(FTP)
        out = []
        for name, data in sorted(TREE.get(self.cwd, {}).items()):
            e = {'Name': dbus.String(name), 'Type': dbus.String('folder' if data is None else 'file'),
                 'Modified': dbus.String('20261009T120000Z')}
            if data is not None:
                e['Size'] = dbus.UInt64(len(data))
            out.append(dbus.Dictionary(e, signature='sv'))
        return dbus.Array(out, signature='a{sv}')

    @dbus.service.method(FTP, in_signature='ss', out_signature='oa{sv}')
    def GetFile(self, target, source):
        self.need(FTP)
        data = TREE.get(self.cwd, {}).get(source)
        if data is None:
            raise err('Failed', 'Not Found')
        return self.transfer(source, data, target)

    @dbus.service.method(FTP, in_signature='ss', out_signature='oa{sv}')
    def PutFile(self, source, target):
        self.need(FTP)
        data = open(source, 'rb').read()
        TREE[self.cwd][target] = data
        log('ftp put %s/%s (%d bytes)' % (self.cwd, target, len(data)))
        return self.transfer(target, data, source)

    @dbus.service.method(FTP, in_signature='s')
    def Delete(self, name):
        self.need(FTP)
        if name not in TREE.get(self.cwd, {}):
            raise err('Failed', 'Not Found')
        del TREE[self.cwd][name]

    @dbus.service.method(FTP, in_signature='ss')
    def CopyFile(self, a, b):
        self.need(FTP)
        TREE[self.cwd][b] = TREE[self.cwd][a]

    @dbus.service.method(FTP, in_signature='ss')
    def MoveFile(self, a, b):
        self.need(FTP)
        TREE[self.cwd][b] = TREE[self.cwd].pop(a)

    # ---- PhonebookAccess1 ----
    @dbus.service.method(PBAP, in_signature='ss')
    def Select(self, location, phonebook):
        self.need(PBAP)
        if phonebook not in ('pb', 'ich', 'och', 'mch', 'cch', 'spd', 'fav'):
            raise err('InvalidArguments', 'Invalid arguments in method call')
        self.pb = phonebook

    def book(self):
        return CONTACTS if self.pb == 'pb' else CONTACTS[1:2]

    @dbus.service.method(PBAP, in_signature='sa{sv}', out_signature='oa{sv}')
    def PullAll(self, target, filters):
        self.need(PBAP)
        return self.transfer('telecom/%s.vcf' % self.pb, b''.join(vcard(n, t) for _, n, t in self.book()), target)

    @dbus.service.method(PBAP, in_signature='a{sv}', out_signature='a(ss)')
    def List(self, filters):
        self.need(PBAP)
        return dbus.Array([dbus.Struct((h, n), signature='ss') for h, n, _ in self.book()], signature='(ss)')

    @dbus.service.method(PBAP, in_signature='ssa{sv}', out_signature='oa{sv}')
    def Pull(self, handle, target, filters):
        self.need(PBAP)
        for h, n, t in self.book():
            if h == handle:
                return self.transfer(h, vcard(n, t), target)
        raise err('Failed', 'Not Found')

    @dbus.service.method(PBAP, in_signature='ssa{sv}', out_signature='a(ss)')
    def Search(self, field, value, filters):
        self.need(PBAP)
        return dbus.Array([dbus.Struct((h, n), signature='ss') for h, n, t in self.book()
                           if value.lower() in (n if field == 'name' else t).lower()], signature='(ss)')

    @dbus.service.method(PBAP, in_signature='', out_signature='q')
    def GetSize(self):
        self.need(PBAP)
        return dbus.UInt16(len(self.book()))

    # ---- MessageAccess1 ----
    @dbus.service.method(MAP, in_signature='s')
    def SetFolder(self, folder):
        self.need(MAP)
        self.map_folder = '' if folder in ('', '/') else folder.strip('/')

    @dbus.service.method(MAP, in_signature='a{sv}', out_signature='aa{sv}')
    def ListFolders(self, filters):
        self.need(MAP)
        names = ['telecom'] if self.map_folder == '' else ['msg'] if self.map_folder == 'telecom' else FOLDER_MAP
        return dbus.Array([dbus.Dictionary({'Name': dbus.String(n)}, signature='sv') for n in names], signature='a{sv}')

    @dbus.service.method(MAP, in_signature='sa{sv}', out_signature='a{oa{sv}}')
    def ListMessages(self, folder, filters):
        self.need(MAP)
        name = (folder or self.map_folder).split('/')[-1]
        for m in self.messages:
            self.svc.remove(m)
        self.messages = []
        out = {}
        for i, (sender, addr, subject, body, read) in enumerate(MESSAGES.get(name, [])):
            props = {'Folder': dbus.String('/telecom/msg/' + name), 'Subject': dbus.String(subject),
                     'Timestamp': dbus.String('20261009T1%d0000' % i), 'Sender': dbus.String(sender),
                     'SenderAddress': dbus.String(addr), 'Type': dbus.String('sms-gsm'),
                     'Size': dbus.UInt64(len(body)), 'Read': dbus.Boolean(read), 'Status': dbus.String('complete')}
            m = Message(self.svc, '%s/message%d' % (self.path, 1000 + i), self, props, body)
            self.messages.append(m)
            out[dbus.ObjectPath(m.path)] = dbus.Dictionary(props, signature='sv')
        return dbus.Dictionary(out, signature='oa{sv}')

    # ---- Image1 (AVRCP cover art) ----
    @dbus.service.method(IMAGE, in_signature='s', out_signature='aa{sv}')
    def Properties(self, handle):
        self.need(IMAGE)
        return dbus.Array([
            dbus.Dictionary({'handle': dbus.String(handle), 'name': dbus.String('cover')}, signature='sv'),
            dbus.Dictionary({'type': dbus.String('native'), 'encoding': dbus.String('PNG'),
                             'pixel': dbus.String('300*300'), 'size': dbus.UInt32(4096)}, signature='sv'),
            dbus.Dictionary({'type': dbus.String('variant'), 'encoding': dbus.String('JPEG'),
                             'pixel': dbus.String('200*200')}, signature='sv'),
        ], signature='a{sv}')

    def cover(self, handle, size):
        # One colour per track, so a track change shows a different image.
        colours = [(30, 90, 200), (200, 60, 60), (40, 150, 80), (160, 120, 30)]
        return png(size, size, colours[int(handle) % len(colours)])

    @dbus.service.method(IMAGE, in_signature='ss', out_signature='oa{sv}')
    def GetThumbnail(self, target, handle):
        self.need(IMAGE)
        return self.transfer('cover-%s-thumb' % handle, self.cover(handle, 200), target)

    @dbus.service.method(IMAGE, in_signature='ssa{sv}', out_signature='oa{sv}')
    def Get(self, target, handle, description):
        self.need(IMAGE)
        return self.transfer('cover-%s' % handle, self.cover(handle, 300), target)


class Message(Obj):
    def __init__(self, svc, path, session, props, body):
        super().__init__(svc, path, {MESSAGE: props})
        self.session = session
        self.body = body
        svc.add(self)

    @dbus.service.method(MESSAGE, in_signature='sb', out_signature='oa{sv}')
    def Get(self, target, attachment):
        p = self.ifaces[MESSAGE]
        bmsg = ('BEGIN:BMSG\r\nVERSION:1.0\r\nSTATUS:READ\r\nTYPE:SMS_GSM\r\nFOLDER:%s\r\nBEGIN:VCARD\r\nVERSION:2.1\r\n'
                'N:%s\r\nTEL:%s\r\nEND:VCARD\r\nBEGIN:BENV\r\nBEGIN:BBODY\r\nLENGTH:%d\r\nBEGIN:MSG\r\n%s\r\nEND:MSG\r\n'
                'END:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n' % (p['Folder'], p['Sender'], p['SenderAddress'],
                                                         len(self.body) + 22, self.body)).encode()
        return self.session.transfer(self.path.rsplit('/', 1)[1], bmsg, target)


class ObexRoot(dbus.service.Object):
    """/: the ObjectManager; /org/bluez/obex: Client1 and AgentManager1."""

    def __init__(self, svc):
        super().__init__(svc.conn, '/')
        self.svc = svc

    @dbus.service.method(OBJMGR, in_signature='', out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        return dbus.Dictionary({dbus.ObjectPath(p): dbus.Dictionary(
            {i: dbus.Dictionary(v, signature='sv') for i, v in o.ifaces.items()}, signature='sa{sv}')
            for p, o in self.svc.objects.items()}, signature='oa{sa{sv}}')

    @dbus.service.signal(OBJMGR, signature='oa{sa{sv}}')
    def InterfacesAdded(self, path, ifaces):
        pass

    @dbus.service.signal(OBJMGR, signature='oas')
    def InterfacesRemoved(self, path, ifaces):
        pass


class ClientObj(dbus.service.Object):
    def __init__(self, svc):
        super().__init__(svc.conn, '/org/bluez/obex')
        self.svc = svc

    @dbus.service.method(CLIENT, in_signature='sa{sv}', out_signature='o', async_callbacks=('ok', 'fail'))
    def CreateSession(self, dest, opts, ok=None, fail=None):
        target = str(opts.get('Target', '')).lower()
        dest = str(dest).upper()
        if target not in TARGETS:
            return fail(err('InvalidArguments', 'Invalid target'))
        if target not in PROFILES.get(dest, ()) or not self.svc.connected(dest):
            log('CreateSession %s %s: refused' % (dest, target))
            # A missing SDP record or an absent device both take a while to fail, as with obexd.
            GLib.timeout_add(1200, lambda: (fail(err('Failed', 'Unable to find service record'
                                                     if self.svc.connected(dest) else 'Host is down')), False)[1])
            return
        self.svc.nsession += 1
        s = Session(self.svc, self.svc.nsession, dest, target, opts)
        log('CreateSession %s %s -> %s' % (dest, target, s.path))
        GLib.timeout_add(400, lambda: (ok(dbus.ObjectPath(s.path)), False)[1])

    @dbus.service.method(CLIENT, in_signature='o')
    def RemoveSession(self, path):
        o = self.svc.objects.get(str(path))
        if not isinstance(o, Session):
            raise err('InvalidArguments', 'Invalid path')
        for m in o.messages:
            self.svc.remove(m)
        self.svc.remove(o)
        log('RemoveSession', path)

    @dbus.service.method(AGENT_MGR, in_signature='o', sender_keyword='sender')
    def RegisterAgent(self, path, sender=None):
        if self.svc.agent:
            raise err('AlreadyExists', 'Already Exists')
        self.svc.agent = (sender, str(path))
        log('agent registered: %s %s' % (sender, path))

    @dbus.service.method(AGENT_MGR, in_signature='o')
    def UnregisterAgent(self, path):
        self.svc.agent = None


class FakeObex:
    def __init__(self, address, adapter_address, connected):
        """connected(addr) -> bool: whether the fake BlueZ has that device connected."""
        self.conn = dbus.bus.BusConnection(address)
        self.name = dbus.service.BusName(OBEX, self.conn)
        self.adapter_address = adapter_address
        self.connected = connected
        self.objects = {}
        self.nsession = 0
        self.nserver = 0
        self.agent = None
        self.root = ObexRoot(self)
        self.client = ClientObj(self)
        # A bench client that goes away leaves its agent registration behind otherwise.
        self.conn.add_signal_receiver(self.owner_changed, 'NameOwnerChanged', 'org.freedesktop.DBus',
                                      'org.freedesktop.DBus', '/org/freedesktop/DBus')

    def owner_changed(self, name, old, new):
        if self.agent and name == self.agent[0] and not new:
            self.agent = None

    def add(self, o):
        self.objects[o.path] = o
        self.root.InterfacesAdded(o.path, dbus.Dictionary(
            {i: dbus.Dictionary(v, signature='sv') for i, v in o.ifaces.items()}, signature='sa{sv}'))

    def remove(self, o):
        if self.objects.pop(o.path, None) is not None:
            self.root.InterfacesRemoved(o.path, dbus.Array(list(o.ifaces), signature='s'))
            o.remove_from_connection()

    def incoming_push(self, dest, name, size, done):
        """A device pushes a file: ask the agent where to put it (AuthorizePush), then write it."""
        if not self.agent:
            return done('no agent registered: obexd would refuse the push')
        self.nserver += 1
        spath = '/org/bluez/obex/server/session%d' % self.nserver
        sess = Obj(self, spath, {SESSION: {'Source': dbus.String(self.adapter_address),
                                           'Destination': dbus.String(dest),
                                           'Target': dbus.String(TARGETS['opp'][0])}})
        self.add(sess)
        data = (('pushed by %s\n' % dest) * (int(size) // 16 + 1)).encode()[:int(size)]
        tpath = spath + '/transfer1'
        t = Obj(self, tpath, {TRANSFER: {'Status': dbus.String('queued'), 'Name': dbus.String(name),
                                         'Size': dbus.UInt64(len(data)), 'Transferred': dbus.UInt64(0),
                                         'Filename': dbus.String(''), 'Session': dbus.ObjectPath(spath)}})
        self.add(t)
        agent = self.conn.get_object(self.agent[0], self.agent[1])

        def authorized(filename):
            log('agent accepted %s -> %s' % (name, filename))
            self.remove(t)
            real = Transfer(self, tpath, spath, name, data, str(filename),
                            on_done=lambda: GLib.timeout_add(1500, lambda: (self.remove(sess), False)[1]))
            done('accepted: ' + str(filename))
            return real

        def refused(e):
            log('agent refused %s: %s' % (name, e.get_dbus_name()))
            self.remove(t)
            self.remove(sess)
            done('refused: ' + e.get_dbus_name())

        agent.AuthorizePush(dbus.ObjectPath(tpath), dbus_interface='org.bluez.obex.Agent1',
                            reply_handler=authorized, error_handler=refused, timeout=70)
