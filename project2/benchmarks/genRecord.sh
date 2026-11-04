#!/bin/sh
set -eu
# Regenerate fixtures instead of appending to an earlier run.
python3 - <<'PY'
from pathlib import Path
import random
rng = random.Random(416)
folder = Path('record')
folder.mkdir(exist_ok=True)
for index in range(10):
    (folder / str(index)).write_text(''.join(f'{rng.randrange(65536)}\n' for _ in range(4096)))
PY
