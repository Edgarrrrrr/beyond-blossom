"""Verify dependency unpacking with tiny local archives, without network/builds."""
import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

import vendor


class VendorTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.vendor=self.root/'vendor';self.vendor.mkdir()
        self.manifest={'schema':1,'components':{}}
        for name in vendor.KEYS:
            self.write_component(name,[(f'{name}/source.txt',b'local source',None)])
        patch=mock.patch.object(vendor,'VENDOR',self.vendor);patch.start();self.addCleanup(patch.stop)

    def save_manifest(self):
        (self.vendor/'manifest.json').write_text(json.dumps(self.manifest))

    def write_component(self,name,members,expected=None):
        path=self.vendor/f'{name}.tar.gz'
        with tarfile.open(path,'w:gz') as stream:
            for member_name,payload,kind in members:
                item=tarfile.TarInfo(member_name)
                if kind:
                    item.type=kind;item.linkname='../../escape'
                    stream.addfile(item)
                else:
                    item.size=len(payload);stream.addfile(item,io.BytesIO(payload))
        self.manifest['components'][name]={'archive':path.name,
            'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
            'files':expected if expected is not None else {'source.txt':hashlib.sha256(b'local source').hexdigest()},
            'origin':{'revision':'fixture','git_blobs':{'source.txt':'historical-label'}}}
        self.save_manifest()

    def test_materialize_all_and_checked_reuse(self):
        output=self.root/'deps'
        result=vendor.materialize(output)
        self.assertTrue(all(key in result for key in vendor.KEYS.values()))
        self.assertEqual(set(result['identities']),set(vendor.KEYS))
        self.assertTrue((output/'dependencies.json').is_file())
        for name,key in vendor.KEYS.items():
            self.assertEqual((Path(result[key])/'source.txt').read_bytes(),b'local source')
            self.assertNotIn('git_blobs',result['identities'][name]['origin'])
        self.assertEqual(vendor.materialize(output)['manifest_sha256'],result['manifest_sha256'])
        (output/'stim/source.txt').write_text('changed')
        with self.assertRaisesRegex(ValueError,'source changed'):
            vendor.materialize(output)

    def test_archive_hash_and_extracted_file_hash_are_both_verified(self):
        archive=self.vendor/'stim.tar.gz';archive.write_bytes(archive.read_bytes()+b'changed')
        with self.assertRaisesRegex(ValueError,'changed vendored archive'):
            vendor.extract('stim',self.root/'bad-archive')
        self.write_component('stim',[('stim/source.txt',b'wrong file bytes',None)])
        with self.assertRaisesRegex(ValueError,'source changed'):
            vendor.extract('stim',self.root/'bad-file')
        self.assertFalse((self.root/'bad-file').exists())
        self.assertFalse(list(self.root.glob('bad-file.extract-*')))

    def test_unsafe_and_duplicate_tar_members_are_rejected(self):
        variants=[('/outside',None),('stim/../outside',None),('other/source.txt',None),
                  ('stim/link',tarfile.SYMTYPE),('stim/link',tarfile.LNKTYPE)]
        for index,(name,kind) in enumerate(variants):
            with self.subTest(name=name,kind=kind):
                self.write_component('stim',[(name,b'local source',kind)])
                with self.assertRaisesRegex(ValueError,'Unsafe'):
                    vendor.extract('stim',self.root/f'unsafe-{index}')
        self.write_component('stim',[('stim/source.txt',b'local source',None)]*2)
        with self.assertRaisesRegex(ValueError,'Unexpected'):
            vendor.extract('stim',self.root/'duplicate')

    def test_missing_extra_and_symlink_outputs_are_rejected(self):
        destination=vendor.extract('stim',self.root/'existing')
        (destination/'extra').write_text('not in manifest')
        with self.assertRaises(ValueError):vendor.extract('stim',destination)
        (destination/'extra').unlink();(destination/'source.txt').unlink()
        with self.assertRaisesRegex(ValueError,'file set differs'):
            vendor.extract('stim',destination)
        (destination/'source.txt').symlink_to(self.vendor/'manifest.json')
        with self.assertRaisesRegex(ValueError,'Symlink'):
            vendor.extract('stim',destination)
        symlink=self.root/'output-link';symlink.symlink_to(destination,target_is_directory=True)
        with self.assertRaisesRegex(ValueError,'must not be a symlink'):
            vendor.extract('stim',symlink)

    def test_manifest_shape_and_requested_names_are_checked(self):
        for names in ([],['stim','stim'],['not-a-component']):
            with self.assertRaisesRegex(ValueError,'distinct known'):
                vendor.materialize(self.root/'selection',names)
        self.manifest['components'].pop('stim');self.save_manifest()
        with self.assertRaisesRegex(ValueError,'incomplete vendor manifest'):
            vendor.load_manifest()


if __name__=='__main__':unittest.main()
