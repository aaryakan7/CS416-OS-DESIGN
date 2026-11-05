"""Exercise real FUSE requests, unmount, then check persisted data on remount."""
import errno
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

binary = (Path(__file__).resolve().parents[1] / 'rufs').resolve()
unmount = shutil.which('fusermount') or shutil.which('fusermount3')
if not unmount:
    raise RuntimeError('Install the FUSE unmount utility before running this test')


def expect_error(code, operation):
    try:
        operation()
    except OSError as error:
        assert error.errno == code, (error, code)
    else:
        raise AssertionError(f'Expected errno {code}')


def check(mount, remount):
    directory = mount / 'nested'
    filename = directory / 'data'
    payload = bytes((i * 37) % 256 for i in range(12000))
    if not remount:
        directory.mkdir(mode=0o750)
        fd = os.open(filename, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o640)
        try:
            assert os.pwrite(fd, payload, 4093) == len(payload)
            assert os.pread(fd, 4093 + len(payload), 0) == b'\0' * 4093 + payload
            assert os.pwrite(fd, b'', 60000) == 0
        finally:
            os.close(fd)
        for i in range(70):
            fd = os.open(directory / f'f{i:03}', os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
            os.close(fd)
        expect_error(errno.EEXIST, lambda: directory.mkdir())
        expect_error(errno.ENOTDIR, lambda: (filename / 'child').mkdir())
        expect_error(errno.ENAMETOOLONG, lambda: os.open(directory / ('a' * 208), os.O_CREAT | os.O_WRONLY, 0o644))
        os.utime(filename, (1700000000, 1700000001))
    assert filename.stat().st_size == 4093 + len(payload)
    assert filename.stat().st_mtime == 1700000001
    assert directory.stat().st_nlink == 2
    assert mount.stat().st_nlink == 3
    assert set(os.listdir(directory)) == {'data'} | {f'f{i:03}' for i in range(70)}
    assert filename.read_bytes() == b'\0' * 4093 + payload
    print('PASS mounted persistence' if remount else 'PASS mounted file operations', flush=True)


with tempfile.TemporaryDirectory(prefix='rufs-mounted-') as temporary:
    folder = Path(temporary)
    mount = folder / 'mount'
    mount.mkdir()
    for remount in (False, True):
        with (folder / 'fuse.log').open('w+') as log:
            process = subprocess.Popen([str(binary), '-f', str(mount)], cwd=folder,
                                       stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 15
                while not os.path.ismount(mount):
                    if process.poll() is not None or time.monotonic() > deadline:
                        log.seek(0)
                        raise RuntimeError('FUSE mount failed\n' + log.read())
                    time.sleep(0.1)
                check(mount, remount)
            finally:
                if os.path.ismount(mount):
                    subprocess.run([unmount, '-u', str(mount)], check=True, timeout=15)
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=5)
                if process.returncode:
                    log.seek(0)
                    raise RuntimeError('FUSE process failed\n' + log.read())
