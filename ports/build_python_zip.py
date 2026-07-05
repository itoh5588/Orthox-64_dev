import os
import zipfile
import sys

src_dir = sys.argv[1]
dest_zip = sys.argv[2]

excludes = ['__pycache__', 'test', 'idlelib', 'tkinter', 'turtledemo']

with zipfile.ZipFile(dest_zip, 'w', zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk(src_dir):
        # Exclude directories
        dirs[:] = [d for d in dirs if d not in excludes]
        for f in files:
            if f.endswith('.so') or f.endswith('.pyc') or f.endswith('.pyo'):
                continue
            abs_path = os.path.join(root, f)
            rel_path = os.path.relpath(abs_path, src_dir)
            zf.write(abs_path, rel_path)
print(f"Created {dest_zip}")
