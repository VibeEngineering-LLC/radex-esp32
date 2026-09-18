"""#USB-1: эталонные кадры из трасс RadexDC -> test/host/radex_usb_vectors.txt (строки "<метка>_req|rsp_<имя> hex").
Запрос = заголовок + данные (две OUT-передачи склеиваются), ответ = IN-передача следом; берётся первая пара
каждого вида, для страниц архива — ещё и последняя (<метка>_rsp_arch_page_last).
Usage (из папки ESP32): python <this> t02=captures/radex_usb_02_archive.pcap t03=... t04=... > vectors.txt"""
import subprocess, sys
TSHARK = r'C:\Program Files\Wireshark\tshark.exe'
WANT = {'c20b': 'current', '040c': 'arch_begin', '0d0c': 'arch_hdr', '050c': 'arch_seek', '0e0c': 'arch_page', '0600': 'set_time'}
for arg in sys.argv[1:]:
    tag, pcap = arg.split('=', 1)
    rows = subprocess.run([TSHARK, '-r', pcap, '-Y', 'usb.capdata', '-T', 'fields', '-e', 'usb.endpoint_address',
                           '-e', 'usb.capdata'], capture_output=True, text=True).stdout.split('\n')
    rows = [r.split('\t') for r in rows if '\t' in r]
    seen, last = set(), {}
    for i in range(len(rows) - 2):
        (e0, h), (e1, d), (e2, r) = rows[i], rows[i + 1], rows[i + 2]
        name = WANT.get(d[:4])
        if not (e0 == e1 == '0x01' and e2 == '0x81' and h.startswith('7bff') and name):
            continue
        last[name] = (h + d, r)
        if name not in seen:
            seen.add(name)
            print(f'{tag}_req_{name} {h}{d}\n{tag}_rsp_{name} {r}')
    if 'arch_page' in last:
        print(f'{tag}_req_arch_page_last {last["arch_page"][0]}\n{tag}_rsp_arch_page_last {last["arch_page"][1]}')
