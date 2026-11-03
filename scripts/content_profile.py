"""Prepare explicit offline AppContent metadata; SKU is a probe setting, not a license check."""
import argparse
import json
import struct
from pathlib import Path
from prepare import read_sfo, sfo


LABEL_SIZE = 17
DIRECTORY_SIZE = 256
CONTENT_ID_OFFSET = 20
MAX_ADDONS = 2500


def discover_addons(game, title_id):
    root = game / 'DLC'
    if not root.is_dir():
        return []
    addons = []
    labels = set()
    for folder in sorted(root.iterdir(), key=lambda path: path.name):
        if not folder.is_dir():
            continue
        param = folder / 'sce_sys' / 'param.sfo'
        if not param.is_file():
            continue
        values = read_sfo(param)
        if not values:
            print(f'Ignoring DLC folder {folder.name}: invalid param.sfo')
            continue
        if values.get('CATEGORY') != 'ac':
            continue
        content_id = values.get('CONTENT_ID')
        if not isinstance(content_id, str) or content_id[7:16] != title_id or \
                content_id[16:20] != '_00-' or len(content_id) <= CONTENT_ID_OFFSET:
            print(f'Ignoring DLC folder {folder.name}: CONTENT_ID does not match {title_id}')
            continue
        label = content_id[CONTENT_ID_OFFSET:]
        directory = folder.name
        if not label.isascii() or not 1 <= len(label) < LABEL_SIZE:
            print(f'Ignoring DLC folder {folder.name}: invalid entitlement label')
            continue
        if not directory.isascii() or len(directory.encode('utf-8')) >= DIRECTORY_SIZE or \
                directory in ('.', '..') or '/' in directory or '\\' in directory:
            print(f'Ignoring DLC folder {folder.name}: invalid folder name')
            continue
        if label in labels:
            print(f'Ignoring DLC folder {folder.name}: duplicate entitlement {label}')
            continue
        labels.add(label)
        addons.append({'entitlement': label, 'directory': directory})
        if len(addons) == MAX_ADDONS:
            print(f'Ignoring further DLC folders: maximum of {MAX_ADDONS} packages reached')
            break
    return addons


def prepare(game, out, sku='full'):
    values=sfo((game/'sce_sys/param.sfo').read_bytes())
    params=[values.get(f'USER_DEFINED_PARAM_{i}',0) for i in range(1,5)]
    if any(type(v) is not int or not 0<=v<=0xffffffff for v in params):
        raise ValueError('invalid user-defined parameter')
    addons = discover_addons(game, values.get('TITLE_ID', ''))
    profile=dict(title_id=values.get('TITLE_ID'),sku=sku,sku_source='explicit probe setting',
                 user_params=params,mounted_addons=addons,boot_attr=0)
    out.mkdir(parents=True,exist_ok=True)
    data = bytearray(struct.pack('<8s6I',b'TFCONT02',{'full':3,'trial':1}[sku],*params,len(addons)))
    for addon in addons:
        label = addon['entitlement'].encode('ascii')
        directory = addon['directory'].encode('utf-8')
        data.extend(label.ljust(LABEL_SIZE, b'\0'))
        data.extend(directory.ljust(DIRECTORY_SIZE, b'\0'))
    (out/'content.bin').write_bytes(data)
    (out/'content-profile.json').write_text(json.dumps(profile,indent=2)+'\n')
    print(f'AppContent profile: SKU={sku} (probe setting), user params={params}, mounted add-ons={len(addons)}')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('game',type=Path)
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--sku',choices=['full','trial'],default='full')
    a=p.parse_args();prepare(a.game,a.out,a.sku)
