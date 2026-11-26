import json, os, socket, subprocess, sys, time, urllib.request, urllib.error
arm = sys.argv[1]
if arm == 'win':
    # Windows 版が QEMU に渡した引数と、Windows 版 QEMU 同梱のファームウェアで起動する
    os.chdir('/t/run/win/FreeOSAny-1.2-windows')
    args = open('/t/out/winargs.txt').read().splitlines()
    log = open('/t/out/launchwin.log', 'w'); log.write('localhost:8080/\n'); log.flush()
    p = subprocess.Popen(['qemu-system-x86_64', *args, '-display', 'none', '-qmp', 'unix:/tmp/q,server,nowait'], stdout=log, stderr=log)
    for _ in range(100):
        if os.path.exists('/tmp/q'): break
        time.sleep(0.2)
else:
    os.chdir('/t/run')
    subprocess.run(['rm', '-rf', arm], check=True)
    os.makedirs(arm); os.chdir(arm)
    subprocess.run(['unzip', '-q', '/t/dist/freeos-any-1.3-mac.zip'], check=True)
    os.chdir('FreeOSAny-1.3-mac')
    print('executable:', os.access('FreeOSAny.command', os.X_OK))
    env = dict(os.environ, PATH='/t/bin:' + os.environ['PATH'], FAKE_ARM=arm, FREEOS_QEMU_BIN='/usr/bin',
               FREEOS_QEMU_EXTRA='-display none -qmp unix:/tmp/q,server,nowait')
    log = open('/t/out/launch%s.log' % arm, 'w')
    p = subprocess.Popen(['./FreeOSAny.command'], env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
    for _ in range(100):
        if os.path.exists('/tmp/q'): break
        time.sleep(0.2)
c = socket.socket(socket.AF_UNIX); c.connect('/tmp/q'); f = c.makefile('rw'); f.readline()
def qmp(cmd, **a):
    f.write(json.dumps({'execute': cmd, 'arguments': a}) + '\n'); f.flush()
    while True:
        r = json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
qmp('qmp_capabilities')
def key(k): qmp('human-monitor-command', **{'command-line': 'sendkey ' + k}); time.sleep(0.2)
def typ(s):
    m = {' ': 'spc', '.': 'dot', '/': 'slash', ':': 'shift-semicolon'}
    for ch in s: key(m.get(ch, ch))
    key('ret')
def shot(n): time.sleep(1); qmp('screendump', filename='/t/out/%s_%s.png' % (n, arm), format='png')
time.sleep(40); shot('boot'); key('f4'); time.sleep(1); key('f5'); time.sleep(1)
typ('net'); time.sleep(10)
typ('get https://example.com/ a.txt'); time.sleep(25); shot('get')
key('f9'); time.sleep(2); key('ret'); time.sleep(8); shot('transfer')
port = [l for l in open(log.name) if 'localhost:' in l][0].split('localhost:')[1].split('/')[0]
try: print(urllib.request.urlopen('http://127.0.0.1:%s/000000/' % port, timeout=30).status)
except urllib.error.HTTPError as e: print('port', port, 'wrong PIN ->', e.code)
except Exception as e: print('transfer failed', e)
# ここからはマウスだけで操作する
def ev(*e): qmp('input-send-event', events=list(e)); time.sleep(0.4)
def move(x, y): ev({'type': 'abs', 'data': {'axis': 'x', 'value': int((x + 0.5) * 32768 / 1280)}}, {'type': 'abs', 'data': {'axis': 'y', 'value': int((y + 0.5) * 32768 / 800)}})
def btn(d): ev({'type': 'btn', 'data': {'button': 'left', 'down': d}})
def click(x, y): move(x, y); btn(True); btn(False); time.sleep(1)
click(40, 782); move(100, 641); shot('m_menu')
click(100, 641); time.sleep(2); shot('m_sol')
move(200, 36); btn(True); move(450, 150); move(700, 330); btn(False); shot('m_drag')
click(40, 782); move(100, 745)
try: btn(True)
except BrokenPipeError: pass
try: print('exit', p.wait(20))
except subprocess.TimeoutExpired: print('still running'); p.kill(); subprocess.run(['pkill', 'qemu'])
