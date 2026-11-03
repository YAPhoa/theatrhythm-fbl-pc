from paths import ROOT
from pathlib import Path
import json
import struct
import tempfile
import unittest
from unittest.mock import patch
import content_profile

class ProfileTests(unittest.TestCase):
    def test_sfo_parameters_and_explicit_trial_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':13,'USER_DEFINED_PARAM_4':0xffffffff}):
                content_profile.prepare(p,p,'trial')
            self.assertEqual(struct.unpack('<8s6I',(p/'content.bin').read_bytes()),(b'TFCONT02',1,13,0,0,0xffffffff,0))

    def test_discovers_matching_unpacked_addons_and_writes_mount_metadata(self):
        base={'TITLE_ID':'CUSA32853','USER_DEFINED_PARAM_1':13}
        addon={'CATEGORY':'ac','CONTENT_ID':'EP0082-CUSA32853_00-DLCPACK000000100'}
        unrelated={'CATEGORY':'ac','CONTENT_ID':'EP0082-CUSA99999_00-OTHER000000000'}
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)
            (p/'sce_sys').mkdir()
            (p/'sce_sys/param.sfo').write_bytes(b'base')
            for name in ('DLCPACK000000100','OTHER'):
                (p/'DLC'/name/'sce_sys').mkdir(parents=True)
                (p/'DLC'/name/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value=base), \
                    patch.object(content_profile,'read_sfo',side_effect=[addon,unrelated]):
                content_profile.prepare(p,p)
            data=(p/'content.bin').read_bytes()
            self.assertEqual(struct.unpack('<8s6I',data[:32]),(b'TFCONT02',3,13,0,0,0,1))
            label=data[32:49].split(b'\0',1)[0].decode()
            directory=data[49:305].split(b'\0',1)[0].decode()
            self.assertEqual((label,directory),('DLCPACK000000100','DLCPACK000000100'))
            self.assertEqual(json.loads((p/'content-profile.json').read_text())['mounted_addons'],
                             [{'entitlement':label,'directory':directory}])

    def test_invalid_sfo_parameter_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':'13'}):
                with self.assertRaisesRegex(ValueError,'invalid user-defined'):
                    content_profile.prepare(p,p)
