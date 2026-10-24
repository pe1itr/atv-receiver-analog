#!/usr/bin/env python3
"""Poll an ATV receiver and emit one JSON object per line for analysis."""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('host', help='Receiver laptop IP address, e.g. 192.168.1.50')
parser.add_argument('--port', type=int, default=8765)
parser.add_argument('--interval', type=float, default=1.0, help='Seconds between polls')
parser.add_argument('--once', action='store_true')
args = parser.parse_args()
if not 1 <= args.port <= 65535 or not 0.1 <= args.interval <= 86400:
    parser.error('Use port 1..65535 and interval 0.1..86400 seconds')
# Connect directly to the receiver, even on laptops with HTTP proxy settings.
client = urllib.request.build_opener(urllib.request.ProxyHandler({}))
try:
    while True:
        try:
            with client.open(f'http://{args.host}:{args.port}/status', timeout=3) as response:
                status = json.load(response)
            print(json.dumps(status, allow_nan=False), flush=True)
        except (OSError, ValueError, urllib.error.URLError) as error:
            print(f'Status unavailable: {error}', file=sys.stderr, flush=True)
            if args.once:
                sys.exit(1)
        if args.once:
            break
        time.sleep(args.interval)
except KeyboardInterrupt:
    pass
