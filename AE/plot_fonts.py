"""Use the redistributable font family included with Matplotlib."""
import hashlib
from pathlib import Path

def register_bundled_fonts(directory=None):
    import matplotlib
    from matplotlib import font_manager
    selected = {}
    for weight in ('normal', 'bold'):
        path = Path(font_manager.findfont(font_manager.FontProperties(family='DejaVu Serif', weight=weight), fallback_to_default=False)).resolve()
        selected[weight] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(), family='DejaVu Serif')
    return dict(matplotlib=matplotlib.__version__, selected=selected)
