"""QEMU+OVMFで起動し、キー入力して画面を撮る。使い方: vm.py disk.img script.py"""
import json, os, socket, subprocess, sys, time

S = os.path.dirname(os.path.abspath(__file__))
R = S + '/root'
img, script = sys.argv[1], sys.argv[2]
sock = '/tmp/qmp.sock'
if os.path.exists(sock):
    os.unlink(sock)
subprocess.run(['cp', '/usr/share/AAVMF/AAVMF_VARS.fd', S + '/vars.fd'], check=True)
p = subprocess.Popen([
    'qemu-system-aarch64', '-machine', 'virt', '-accel', 'tcg', '-cpu', 'cortex-a72', '-m', '512', '-display', 'none', '-netdev', 'user,id=n', '-device', 'virtio-net-pci,netdev=n,romfile=', '-device', 'virtio-rng-pci',
    '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd',
    '-drive', f'if=pflash,format=raw,file={S}/vars.fd', '-device', 'virtio-gpu-pci',
    '-device', 'qemu-xhci', '-drive', f'if=none,id=d,format=raw,file={img}', '-device', 'usb-storage,drive=d',
    '-device', 'usb-kbd', '-qmp', f'unix:{sock},server,nowait'])
try:
    for _ in range(100):
        if os.path.exists(sock):
            break
        time.sleep(0.1)
    c = socket.socket(socket.AF_UNIX)
    c.connect(sock)
    f = c.makefile('rw')
    f.readline()

    def qmp(cmd, **args):
        f.write(json.dumps({'execute': cmd, 'arguments': args}) + '\n')
        f.flush()
        while True:
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r:
                if 'error' in r:
                    print(r)
                return r

    qmp('qmp_capabilities')

    def hmp(line):
        return qmp('human-monitor-command', **{'command-line': line})

    def key(k, wait=0.15):
        hmp('sendkey ' + k)
        time.sleep(wait)

    names = {':': 'shift-semicolon', '_': 'shift-minus', ' ': 'spc', '.': 'dot', '-': 'minus', '/': 'slash', '\\': 'backslash', '*': 'shift-8',
             '+': 'shift-equal', '(': 'shift-9', ')': 'shift-0', ',': 'comma', '\n': 'ret'}

    def typ(s):
        for ch in s:
            key(names.get(ch, 'shift-' + ch.lower() if ch.isupper() else ch))

    def shot(name):
        time.sleep(1)
        qmp('screendump', filename=f'{S}/{name}.png', format='png')

    def running():
        return p.poll() is None

    exec(open(script).read())
finally:
    p.terminate()
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill()
