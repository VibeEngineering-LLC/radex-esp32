"""#USB-1: эталонные кадры из трасс RadexDC -> test/host/radex_usb_vectors.txt (строки "имя hex").
Запрос = заголовок + данные (две OUT-передачи склеиваются), ответ = IN-передача следом.
Usage (из папки ESP32): python <this> captures/radex_usb_02_archive.pcap captures/radex_usb_03_settime.pcap > vectors.txt"""
import subprocess, sys
TSHARK = r'C:\Program Files\Wireshark\tshark.exe'
WANT = {'c20b': 'current', '040c': 'arch_begin', '0d0c': 'arch_hdr', '050c': 'arch_seek', '0e0c': 'arch_page', '0600': 'set_time'}
seen = set()
for pcap in sys.argv[1:]:
    rows = subprocess.run([TSHARK, '-r', pcap, '-Y', 'usb.capdata', '-T', 'fields', '-e', 'usb.endpoint_address',
                           '-e', 'usb.capdata'], capture_output=True, text=True).stdout.split('\n')
    rows = [r.split('\t') for r in rows if '\t' in r]
    for i in range(len(rows) - 2):
        (e0, h), (e1, d), (e2, r) = rows[i], rows[i + 1], rows[i + 2]
        name = WANT.get(d[:4])
        if e0 == e1 == '0x01' and e2 == '0x81' and h.startswith('7bff') and name and name not in seen:
            seen.add(name)
            print(f'req_{name} {h}{d}')
            print(f'rsp_{name} {r}')
