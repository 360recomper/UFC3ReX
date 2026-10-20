# Install Xbox 360 DLC (marketplace STFS packages) for the recomp.
#   python -I tools\install_dlc.py <userdata_root> <package|zip|folder> [...]
#   python -I tools\install_dlc.py --list <userdata_root>
# Accepts raw LIVE/PIRS/CON packages, the zips they ship in, or folders to scan.
# The runtime's content manager reads DLC as extracted folders:
#   <userdata>/0000000000000000/<title>/00000002/<package>/...
#   <userdata>/0000000000000000/<title>/Headers/00000002/<package>.header
# The header is XCONTENT_AGGREGATE_DATA (0x148 bytes) plus a license mask; the
# mask is written as all-ones so the game sees every pack as purchased.
import os, shutil, struct, sys, tempfile, zipfile

TITLE_ID = 0x5451087D
MARKETPLACE = 2
LICENSE_ALL = 0xFFFFFFFF
BLOCKS_PER_LEVEL = (170, 170 * 170, 170 * 170 * 170)


def i24le(b): return b[0] | (b[1] << 8) | (b[2] << 16)
def i24be(b): return (b[0] << 16) | (b[1] << 8) | b[2]


class Stfs:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(0x1000)
        if h[:4] not in (b'LIVE', b'PIRS', b'CON '):
            raise ValueError('not an STFS package')
        self.header_size = struct.unpack('>I', h[0x340:0x344])[0]
        self.content_type = struct.unpack('>I', h[0x344:0x348])[0]
        self.title_id = struct.unpack('>I', h[0x360:0x364])[0]
        flags = h[0x37B]
        self.read_only = bool(flags & 1)
        self.shift = 0 if self.read_only else 1  # read-only keeps one hash table copy
        self.ft_count = struct.unpack('<H', h[0x37C:0x37E])[0]
        self.ft_block = i24le(h[0x37E:0x381])
        self.title = h[0x411:0x511].decode('utf-16-be', 'replace').split('\0')[0].strip()
        self.base = (self.header_size + 0xFFF) & ~0xFFF

    def close(self): self.f.close()

    def block_offset(self, idx):
        block = idx
        for lvl in BLOCKS_PER_LEVEL:
            block += ((idx + lvl) // lvl) << self.shift
            if idx < lvl:
                break
        return self.base + (block << 12)

    def hash_table_offset(self, idx):
        # Level-0 hash table covering data block idx (same layout as xenia's
        # BlockToHashBlockNumberSTFS; two-copy packages use the first copy).
        step0 = 170 + (1 << self.shift)
        block = (idx // 170) * step0
        if idx >= 170:
            block += ((idx // (170 * 170)) + 1) << self.shift
        if idx >= 170 * 170:
            block += 1 << self.shift
        return self.base + (block << 12)

    def read_block(self, idx):
        self.f.seek(self.block_offset(idx))
        return self.f.read(0x1000)

    def next_block(self, idx):
        self.f.seek(self.hash_table_offset(idx) + (idx % 170) * 0x18)
        return i24be(self.f.read(0x18)[0x15:0x18])

    def entries(self):
        out = []
        blk_idx = self.ft_block
        for _ in range(self.ft_count):
            blk = self.read_block(blk_idx)
            for e in range(0, 0x1000, 0x40):
                ent = blk[e:e + 0x40]
                flags = ent[0x28]
                n = flags & 0x3F
                if n == 0:
                    continue
                out.append(dict(
                    name=ent[:n].decode('ascii'),
                    contiguous=bool(flags & 0x40), is_dir=bool(flags & 0x80),
                    blocks=i24le(ent[0x29:0x2C]), start=i24le(ent[0x2F:0x32]),
                    parent=struct.unpack('>h', ent[0x32:0x34])[0],
                    size=struct.unpack('>I', ent[0x34:0x38])[0]))
            blk_idx = blk_idx + 1 if self.ft_count == 1 else self.next_block(blk_idx)
        return out

    def extract(self, dest):
        ents = self.entries()

        def relpath(i):
            parts = []
            while i >= 0:
                name = ents[i]['name']
                if name in ('.', '..') or '/' in name or '\\' in name or ':' in name:
                    raise ValueError(f'bad entry name {name!r}')
                parts.append(name)
                i = ents[i]['parent']
            return os.path.join(*reversed(parts))

        for i, e in enumerate(ents):
            p = os.path.join(dest, relpath(i))
            if e['is_dir']:
                os.makedirs(p, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(p), exist_ok=True)
            remaining, blk = e['size'], e['start']
            with open(p, 'wb') as out:
                for b in range(e['blocks']):
                    if remaining <= 0:
                        break
                    chunk = self.read_block(blk)[:min(0x1000, remaining)]
                    out.write(chunk)
                    remaining -= len(chunk)
                    blk = blk + 1 if e['contiguous'] else self.next_block(blk)
            if remaining:
                raise ValueError(f"{e['name']}: short read")
        return ents


def header_bytes(title, file_name, title_id):
    b = bytearray(0x148)
    struct.pack_into('>II', b, 0, 1, MARKETPLACE)  # device_id (HDD), content_type
    nm = title.encode('utf-16-be')[:254]
    b[8:8 + len(nm)] = nm
    fn = file_name.encode('ascii')[:42]
    b[0x108:0x108 + len(fn)] = fn
    struct.pack_into('>QI', b, 0x138, 0, title_id)  # xuid 0 = shared by all profiles
    return bytes(b) + struct.pack('<I', LICENSE_ALL)


def install_package(path, name, userdata):
    pkg = Stfs(path)
    try:
        if pkg.content_type != MARKETPLACE:
            print(f'skip {name}: content type {pkg.content_type:08X}, not DLC')
            return False
        if pkg.title_id != TITLE_ID:
            print(f'skip {name}: title {pkg.title_id:08X}, not UFC 3')
            return False
        title_dir = os.path.join(userdata, '0000000000000000', f'{pkg.title_id:08X}')
        dest = os.path.join(title_dir, f'{MARKETPLACE:08X}', name)
        tmp = dest + '.partial'
        shutil.rmtree(tmp, ignore_errors=True)
        ents = pkg.extract(tmp)
        shutil.rmtree(dest, ignore_errors=True)
        os.replace(tmp, dest)
        hdr_dir = os.path.join(title_dir, 'Headers', f'{MARKETPLACE:08X}')
        os.makedirs(hdr_dir, exist_ok=True)
        with open(os.path.join(hdr_dir, name + '.header'), 'wb') as f:
            f.write(header_bytes(pkg.title, name, pkg.title_id))
        files = [e for e in ents if not e['is_dir']]
        print(f"ok   {pkg.title}  [{name}]  {len(files)} files, "
              f"{sum(e['size'] for e in files) / 2**20:.0f} MB")
        return True
    finally:
        pkg.close()


def is_stfs(path):
    with open(path, 'rb') as f:
        return f.read(4) in (b'LIVE', b'PIRS', b'CON ')


def install_zip(path, userdata):
    n = 0
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            name = os.path.basename(info.filename)
            with tempfile.TemporaryDirectory() as td:
                tmp = os.path.join(td, 'pkg')
                with z.open(info) as src, open(tmp, 'wb') as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                if is_stfs(tmp) and install_package(tmp, name, userdata):
                    n += 1
    return n


def install(path, userdata):
    if os.path.isdir(path):
        return sum(install(os.path.join(r, f), userdata)
                   for r, _, fs in os.walk(path) for f in fs)
    if zipfile.is_zipfile(path):
        return install_zip(path, userdata)
    if is_stfs(path):
        return int(install_package(path, os.path.basename(path), userdata))
    return 0


def list_installed(userdata):
    hdr_dir = os.path.join(userdata, '0000000000000000', f'{TITLE_ID:08X}', 'Headers', '00000002')
    if not os.path.isdir(hdr_dir):
        print('no DLC installed')
        return
    for fn in sorted(os.listdir(hdr_dir)):
        with open(os.path.join(hdr_dir, fn), 'rb') as f:
            h = f.read(0x148)
        print(h[8:0x108].decode('utf-16-be').split('\0')[0], ' ', fn[:-7])


if __name__ == '__main__':
    sys.stdout.reconfigure(errors='replace')
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--list':
        list_installed(args[1])
    elif len(args) >= 2:
        total = 0
        for p in args[1:]:
            got = install(p, args[0])
            if got == 0:
                print(f'warn {p}: no UFC 3 DLC package found')
            total += got
        print(f'installed {total} package(s) into {args[0]}')
    else:
        print(__doc__ or open(__file__).read().split('\nimport')[0])
        sys.exit(1)
