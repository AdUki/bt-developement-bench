#!/usr/bin/env python3
"""A fake UPnP/DLNA media server, for trying the console's UPnP browser without a NAS.

It answers SSDP M-SEARCH for MediaServer/ContentDirectory on every interface, serves a device
description, and a ContentDirectory whose Browse lists two folders of generated WAV files (tones
and noise, made on the fly; nothing is stored). Run it on the PC — or anywhere on the board's LAN:

    tools/fake-upnp/server.py [--port 8200] [--name "Bench Media"]

then "search the LAN" on the console's Audio tab. Plain Python 3, no dependencies.
"""

import argparse
import html
import io
import math
import random
import socket
import struct
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UDN = 'uuid:' + str(uuid.uuid5(uuid.NAMESPACE_DNS, 'btbench-fake-upnp'))
CD = 'urn:schemas-upnp-org:service:ContentDirectory:1'
MS = 'urn:schemas-upnp-org:device:MediaServer:1'

# Containers and items: id -> (parent, title, children | None); items carry their generator.
TREE = {
    '0': ('-1', 'root', ['tones', 'noise']),
    'tones': ('0', 'Tones', ['t440', 't1k', 't10k']),
    'noise': ('0', 'Noise', ['white', 'pink']),
    't440': ('tones', '440 Hz, 10 s', None),
    't1k': ('tones', '1 kHz, 10 s', None),
    't10k': ('tones', '10 kHz, 10 s', None),
    'white': ('noise', 'White noise, 10 s', None),
    'pink': ('noise', 'Pink noise, 10 s', None),
}
FREQ = {'t440': 440, 't1k': 1000, 't10k': 10000}


def wav(item, rate=44100, secs=10):
    n = rate * secs
    out = io.BytesIO()
    out.write(b'RIFF' + struct.pack('<I', 36 + n * 4) + b'WAVEfmt ' +
              struct.pack('<IHHIIHH', 16, 1, 2, rate, rate * 4, 4, 16) + b'data' + struct.pack('<I', n * 4))
    rnd = random.Random(1)
    b = [0.0] * 7
    frames = bytearray()
    for i in range(n):
        if item in FREQ:
            v = 0.25 * math.sin(2 * math.pi * FREQ[item] * i / rate)
        else:
            w = rnd.uniform(-1, 1)
            if item == 'pink':
                b[0] = 0.99886 * b[0] + w * 0.0555179; b[1] = 0.99332 * b[1] + w * 0.0750759
                b[2] = 0.96900 * b[2] + w * 0.1538520; b[3] = 0.86650 * b[3] + w * 0.3104856
                b[4] = 0.55000 * b[4] + w * 0.5329522; b[5] = -0.7616 * b[5] - w * 0.0168980
                w = (sum(b[:6]) + b[6] + w * 0.5362) * 0.11; b[6] = w * 0.115926
            v = 0.2 * w
        s = int(max(-1, min(1, v)) * 32767)
        frames += struct.pack('<hh', s, s)
    out.write(frames)
    return out.getvalue()


def didl(ids, base):
    parts = ['<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
             'xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">']
    for i in ids:
        parent, title, kids = TREE[i]
        if kids is not None:
            parts.append('<container id="%s" parentID="%s" childCount="%d" restricted="1"><dc:title>%s</dc:title>'
                         '<upnp:class>object.container.storageFolder</upnp:class></container>' % (i, parent, len(kids), html.escape(title)))
        else:
            parts.append('<item id="%s" parentID="%s" restricted="1"><dc:title>%s</dc:title><dc:creator>btbench</dc:creator>'
                         '<upnp:album>Fake UPnP</upnp:album><upnp:class>object.item.audioItem.musicTrack</upnp:class>'
                         '<res protocolInfo="http-get:*:audio/wav:*" duration="0:00:10.000">%s/media/%s.wav</res></item>'
                         % (i, parent, html.escape(title), base, i))
    parts.append('</DIDL-Lite>')
    return ''.join(parts)


class Handler(BaseHTTPRequestHandler):
    def base(self):
        return 'http://%s:%d' % (self.connection.getsockname()[0], self.server.server_port)

    def reply(self, body, ctype='text/xml; charset="utf-8"', status=200):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(status)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == '/desc.xml':
            self.reply('<?xml version="1.0"?><root xmlns="urn:schemas-upnp-org:device-1-0"><specVersion><major>1</major><minor>0</minor></specVersion>'
                       '<device><deviceType>%s</deviceType><friendlyName>%s</friendlyName><manufacturer>btbench</manufacturer>'
                       '<modelName>fake-upnp</modelName><UDN>%s</UDN><serviceList><service><serviceType>%s</serviceType>'
                       '<serviceId>urn:upnp-org:serviceId:ContentDirectory</serviceId><controlURL>/cd/control</controlURL>'
                       '<eventSubURL>/cd/event</eventSubURL><SCPDURL>/cd.xml</SCPDURL></service></serviceList></device></root>'
                       % (MS, html.escape(self.server.name), UDN, CD))
        elif self.path.startswith('/media/') and self.path.endswith('.wav') and self.path[7:-4] in TREE:
            self.reply(wav(self.path[7:-4]), 'audio/wav')
        else:
            self.reply('not found', 'text/plain', 404)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode('utf-8', 'replace')
        oid = body.split('<ObjectID>', 1)[-1].split('</ObjectID>', 1)[0]
        if self.path != '/cd/control' or oid not in TREE or TREE[oid][2] is None:
            fault = ('<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><s:Fault><faultcode>s:Client</faultcode>'
                     '<faultstring>UPnPError</faultstring><detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>701</errorCode>'
                     '<errorDescription>No such object</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>')
            return self.reply(fault, status=500)
        kids = TREE[oid][2]
        result = html.escape(didl(kids, self.base()))
        self.reply('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
                   's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:BrowseResponse xmlns:u="%s">'
                   '<Result>%s</Result><NumberReturned>%d</NumberReturned><TotalMatches>%d</TotalMatches><UpdateID>1</UpdateID>'
                   '</u:BrowseResponse></s:Body></s:Envelope>' % (CD, result, len(kids), len(kids)))

    def log_message(self, fmt, *args):
        print('http:', fmt % args, flush=True)


def ssdp(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, 'SO_REUSEPORT'):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.bind(('', 1900))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton('239.255.255.250') + socket.inet_aton('0.0.0.0'))
    while True:
        data, peer = s.recvfrom(2048)
        msg = data.decode('utf-8', 'replace')
        if not msg.startswith('M-SEARCH'):
            continue
        st = next((l.split(':', 1)[1].strip() for l in msg.split('\r\n') if l.lower().startswith('st:')), '')
        if st not in (MS, CD, 'ssdp:all', 'upnp:rootdevice'):
            continue
        # Our address as the asker reaches it: a connected UDP socket picks the right interface.
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(peer)
        me = probe.getsockname()[0]
        probe.close()
        reply = ('HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\nLOCATION: http://%s:%d/desc.xml\r\n'
                 'SERVER: Linux/1 UPnP/1.0 fake-upnp/1\r\nST: %s\r\nUSN: %s::%s\r\n\r\n' % (me, port, st, UDN, st))
        s.sendto(reply.encode(), peer)
        print('ssdp: answered %s for %s' % (peer[0], st), flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8200)
    ap.add_argument('--name', default='Bench Media (fake)')
    a = ap.parse_args()
    srv = ThreadingHTTPServer(('', a.port), Handler)
    srv.name = a.name
    threading.Thread(target=ssdp, args=(a.port,), daemon=True).start()
    print('fake UPnP media server %s on :%d' % (UDN, a.port), flush=True)
    srv.serve_forever()


if __name__ == '__main__':
    main()
