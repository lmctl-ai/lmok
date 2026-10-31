#!/usr/bin/env python3
"""Same-length edits within one second must invalidate the persisted facts."""
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

host = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='lmok-index-stat-') as directory:
    root = Path(directory)
    source, db = root / 'source.c', root / 'index.db'
    second = 1_800_000_000 * 1_000_000_000

    def write(text, offset):
        source.write_bytes(text.encode('utf-8'))
        os.utime(source, ns=(second + offset, second + offset))
        return source.stat().st_mtime_ns

    def index(*extra):
        result = subprocess.run([host, 'index', str(source), '--db', str(db), *extra],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, (result.returncode, result.stderr)
        return result.stdout.strip()

    first = write('int before;\n', 100_000_000)
    assert index() == 'indexed 1 files (0 unchanged, 0 removed)'
    second_stamp = write('int after_;\n', 200_000_000)
    assert first != second_stamp and first // 1_000_000_000 == second_stamp // 1_000_000_000
    assert index() == 'indexed 1 files (0 unchanged, 0 removed)'
    with sqlite3.connect(db) as con:
        assert con.execute('SELECT name FROM symbols').fetchall() == [('after_',)]
        assert con.execute('SELECT text FROM file_text').fetchone()[0] == 'int after_;\n'
    assert index() == 'indexed 0 files (1 unchanged, 0 removed)'
    # Touch-only changes must refresh stat data without re-extracting.
    os.utime(source, ns=(second + 300_000_000, second + 300_000_000))
    assert index() == 'indexed 0 files (1 unchanged, 0 removed)'
    # Existing DBs with whole-second stamps get a one-time hash refresh.
    with sqlite3.connect(db) as con:
        con.execute('UPDATE files SET mtime = mtime / 1000000000')
    assert index() == 'indexed 0 files (1 unchanged, 0 removed)'
    assert index('--force') == 'indexed 1 files (0 unchanged, 0 removed)'
print('index: subsecond edits, stat/hash skips, legacy timestamps and --force verified')
