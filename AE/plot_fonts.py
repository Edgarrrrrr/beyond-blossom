"""Verify and select the locally bundled fonts used by the paper."""
import hashlib
import json
from pathlib import Path

def register_paper_fonts(directory=None):
    import matplotlib
    from matplotlib import font_manager
    root = Path(directory) if directory else Path(__file__).resolve().parent / 'fonts'
    manifest = json.loads((root / 'manifest.json').read_text())
    # Select the checked-in files even if different versions are installed locally.
    families = {'Arial', 'Times New Roman'}
    font_manager.fontManager.ttflist[:] = [f for f in font_manager.fontManager.ttflist if f.name not in families]
    for name, expected in manifest['files'].items():
        path = root / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError('Paper font checksum mismatch: ' + name)
        font_manager.fontManager.addfont(str(path))
    font_manager.fontManager._findfont_cached.cache_clear()
    selected = {}
    for family in sorted(families):
        selected[family] = {}
        for weight in ('normal', 'bold'):
            path = Path(font_manager.findfont(font_manager.FontProperties(family=family, weight=weight), fallback_to_default=False)).resolve()
            if not path.is_relative_to(root.resolve()):
                raise ValueError('Paper font was resolved outside the repository')
            selected[family][weight] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(), family=family)
    return dict(matplotlib=matplotlib.__version__, selected=selected)


register_bundled_fonts = register_paper_fonts
