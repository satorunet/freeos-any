import json, os, shutil, socket, subprocess, time
APP = '/tmp/Applications/FreeOS Any.app'
shutil.copytree('/t/dmg/FreeOS Any.app', APP)
exe = APP + '/Contents/MacOS/FreeOSAny'
LOG = '/t/out/dialog.log'
def env(**kw):
    e = dict(os.environ, PATH='/t/macfake:' + os.environ['PATH'], FREEOS_BIN='/usr/bin', FREEOS_DATA='/tmp/Library/Application Support/FreeOS Any',
             FREEOS_QEMU_EXTRA='-display none -qmp unix:/tmp/q,server,nowait')
    e.update(kw); return e
def log():
    s = open(LOG).read() if os.path.exists(LOG) else ''
    open(LOG, 'w').close(); return s.strip()
def boot(label, keys, **kw):
    if os.path.exists('/tmp/q'): os.unlink('/tmp/q')
    p = subprocess.Popen([exe], env=env(**kw), cwd='/')
    for _ in range(200):
        if os.path.exists('/tmp/q'): break
        time.sleep(0.2)
    time.sleep(1)
    if p.poll() is not None: print(label, 'EXITED', p.returncode, log()); return
    c = socket.socket(socket.AF_UNIX); c.connect('/tmp/q'); f = c.makefile('rw'); f.readline()
    def qmp(cmd, **a):
        f.write(json.dumps({'execute': cmd, 'arguments': a}) + '\n'); f.flush()
        while True:
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r: return r
    qmp('qmp_capabilities')
    def key(k): qmp('human-monitor-command', **{'command-line': 'sendkey ' + k}); time.sleep(0.2)
    time.sleep(40)
    # 二重起動
    r = subprocess.run([exe], env=env(), cwd='/', timeout=30); print(label, 'second launch rc', r.returncode, '|', log())
    keys(key); time.sleep(3)
    qmp('screendump', filename='/t/out/app_%s.png' % label, format='png')
    def ev(*e): qmp('input-send-event', events=list(e)); time.sleep(0.4)
    def move(x, y): ev({'type': 'abs', 'data': {'axis': 'x', 'value': int((x+.5)*32768/1280)}}, {'type': 'abs', 'data': {'axis': 'y', 'value': int((y+.5)*32768/800)}})
    def btn(d): ev({'type': 'btn', 'data': {'button': 'left', 'down': d}})
    move(40, 782); btn(True); btn(False); time.sleep(1); move(100, 745)
    try: btn(True)
    except BrokenPipeError: pass
    try: print(label, 'exit', p.wait(20), '|', log())
    except subprocess.TimeoutExpired: print(label, 'still running'); p.kill(); subprocess.run(['pkill', 'qemu'])
def typ(key, s):
    for ch in s: key({' ': 'spc', '.': 'dot'}.get(ch, ch))
    key('ret')
def first(key): key('f4'); time.sleep(1); key('f5'); typ(key, 'write memo.txt hello'); typ(key, 'ls')
def later(key): key('f4'); time.sleep(1); key('f5'); typ(key, 'cat memo.txt'); typ(key, 'net')
log()
arm = os.environ.get('ARM', '0')
# QEMU も Homebrew もない
r = subprocess.run([exe], env=env(FREEOS_BIN='/t/empty'), timeout=30); print('noqemu rc', r.returncode, '|', log())
boot('first' + arm, first, FAKE_ARM=arm)
boot('hvfallback' + arm, later, FAKE_ARM=arm, FAKE_HV='1')
open(APP + '/Contents/Resources/VERSION', 'w').write('9.9\n')
boot('updatefail' + arm, later, FAKE_ARM=arm, FAKE_HDI_FAIL='1')
D = '/tmp/Library/Application Support/FreeOS Any/'
print('version after fail', open(D + 'disk-version').read().strip())
boot('update' + arm, later, FAKE_ARM=arm)
print('version after update', open(D + 'disk-version').read().strip(), sorted(os.listdir(D)))
print(open(D + 'qemu-hvf.log').read()[:200] if os.path.exists(D + 'qemu-hvf.log') else 'no hvf log')
