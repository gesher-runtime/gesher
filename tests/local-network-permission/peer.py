# SPDX-License-Identifier: Apache-2.0
import selectors
import socket
import sys

REQUEST = bytes.fromhex('47657368')
RESPONSE = bytes.fromhex('65725141')

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as tcp, \
        socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp, selectors.DefaultSelector() as events:
    tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp.bind(('127.0.0.1', int(sys.argv[1])))
    address = tcp.getsockname()
    tcp.listen()
    udp.bind(address)
    events.register(tcp, selectors.EVENT_READ)
    events.register(udp, selectors.EVENT_READ)
    print('READY', *address, flush=True)
    while True:
        for event, _ in events.select():
            if event.fileobj is tcp:
                client, _ = tcp.accept()
                with client, client.makefile('rb') as stream:
                    client.settimeout(2)
                    try:
                        if stream.read(4) == REQUEST:
                            client.sendall(RESPONSE)
                            print('TCP REQUEST', flush=True)
                    except OSError:
                        continue
            else:
                payload, sender = udp.recvfrom(64)
                if payload == REQUEST:
                    udp.sendto(RESPONSE, sender)
                    print('UDP REQUEST', flush=True)
