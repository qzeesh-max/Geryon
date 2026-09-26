import re
from collections import defaultdict

def parse_log(filename):
    if not open(filename).read(): return None
    stats = defaultdict(lambda: {'sent': [], 'recv': [], 'tx_time': [], 'tx_between': []})
    
    with open(filename, 'r') as f:
        for line in f:
            if '[Replica ' in line:
                m = re.search(r'\[Replica (\d+)\] Pages Sent: (\d+), Pages Received: (\d+) \| Avg Transfer Time: ([\d.]+) us \| Avg Time Between Transfers: ([\d.]+) us', line)
                if m:
                    idx, sent, recv, tx_time, tx_between = m.groups()
                    stats[int(idx)]['sent'].append(float(sent))
                    stats[int(idx)]['recv'].append(float(recv))
                    stats[int(idx)]['tx_time'].append(float(tx_time))
                    stats[int(idx)]['tx_between'].append(float(tx_between))
            elif '[Primary]' in line:
                m = re.search(r'\[Primary\] Pages Sent: (\d+), Pages Received: (\d+) \| Avg Transfer Time: ([\d.]+) us \| Avg Time Between Transfers: ([\d.]+) us', line)
                if m:
                    sent, recv, tx_time, tx_between = m.groups()
                    stats[0]['sent'].append(float(sent))
                    stats[0]['recv'].append(float(recv))
                    stats[0]['tx_time'].append(float(tx_time))
                    stats[0]['tx_between'].append(float(tx_between))
    return stats

def print_stats(stats, platform):
    print(f"\n{platform} Stats:")
    for k in sorted(stats.keys()):
        s = stats[k]
        if not s['sent']: continue
        avg_sent = sum(s['sent']) / len(s['sent'])
        avg_recv = sum(s['recv']) / len(s['recv'])
        avg_tx = sum(s['tx_time']) / len(s['tx_time'])
        avg_btw = sum(s['tx_between']) / len(s['tx_between'])
        name = 'Primary' if k == 0 else f'Replica {k}'
        print(f"{name}: Sent={avg_sent:.1f}, Recv={avg_recv:.1f}, TxTime={avg_tx:.2f}us, TxBetween={avg_btw:.2f}us")

mac = parse_log('mac_runs.log')
win = parse_log('win_runs.log')

if mac: print_stats(mac, "macOS")
if win: print_stats(win, "Windows")

