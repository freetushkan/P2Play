#!/usr/bin/env python3
"""Real isolated two-node v2 transfer (no Python libtorrent dependency).
Usage: python3 -I tools/mesh-selective-test.py /absolute/path/to/p2playd
KEEP=1 retains logs. Media retrieval is whole-pack, not exact slice/range transfer.
"""
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import sys
import tempfile
import time
import urllib.error
import urllib.request

helpers = runpy.run_path(str(Path(__file__).with_name('peer-items-test.py')))
Node, wait, TOKEN = (helpers[k] for k in ('Node', 'wait', 'TOKEN'))


def media(node, path, data=None, content_type=None):
    request = urllib.request.Request(f'http://127.0.0.1:{node.http}' + path, data=data,
                                    headers={'Authorization': 'Bearer ' + TOKEN})
    if content_type:
        request.add_header('Content-Type', content_type)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status, response.read(), response.headers
    except urllib.error.HTTPError as e:
        return e.code, e.read(), e.headers


def main():
    root = tempfile.mkdtemp(prefix='p2play-selective-')
    nodes = []
    try:
        a = Node(os.path.abspath(sys.argv[1]), root, 'a'); nodes.append(a)
        b = Node(os.path.abspath(sys.argv[1]), root, 'b'); nodes.append(b)
        pk = a.call('/api/publish', {'name': 'Selective network'})[1]['pubkey']
        description = b'Lazy authenticated description, never put into search text.'
        poster = b'\xff\xd8\xff' + os.urandom(220000)
        ref = json.loads(media(a, f'/api/publish/{pk}/media/description', description,
                              'text/plain; charset=utf-8')[1])
        image = json.loads(media(a, f'/api/publish/{pk}/media/poster', poster, 'image/jpeg')[1])
        records = [{'ih': 'a'*40, 'title': 'Selected film', 'shard': 'movies',
                    'description_ref': ref, 'poster_ref': image},
                   {'ih': 'b'*40, 'title': 'Unselected film', 'shard': 'books'}]
        assert a.call(f'/api/publish/{pk}/records', raw='\n'.join(map(json.dumps, records)).encode())[0] == 200
        release = a.call(f'/api/publish/{pk}/release', {})
        assert release[0] == 200, release
        original_hash = release[1]['net']['version_ih']
        assert b.call(f'/api/lists/{pk}/policy', {'shards':['movies'], 'media_bytes': 1048576})[0] == 200
        assert b.call(f'/api/lists/{pk}', {'peers':[f'127.0.0.1:{a.bt}']})[0] == 202
        def indexed():
            results = b.call('/api/catalog/search')[1]['results']
            return results if any(r['hash'] == 'a'*40 for r in results) else None
        results = wait(indexed, 75)
        assert not any(r['hash'] == 'b'*40 for r in results), results
        plan = b.call(f'/api/lists/{pk}/files')[1]['files']
        assert all(f['present'] == f['selected'] for f in plan if f['kind'] == 'records'), plan
        assert not any(f['present'] for f in plan if f['kind'] != 'records'), plan
        network = Path(b.data, 'lists', '.network')
        assert not list(network.rglob(image['pack'])) and not list(network.rglob(ref['pack']))
        held = b.lists()[0]
        assert held['net']['version_ih'] == original_hash and held['net']['selective']
        print('PASS real v2 transfer: authenticated manifest, selected shard only, media lazy')
        path = f'/api/catalog/{"a"*40}/description?list={pk}'
        status, _, headers = media(b, path)
        assert status == 503 and headers['Retry-After'] == '2', status
        wait(lambda: media(b, path)[1] == description, 65)
        cache = Path(b.data, 'mesh-local', pk, 'cache', hashlib.sha256(description).hexdigest())
        assert cache.read_bytes() == description
        assert not Path(b.data, 'lists', pk, 'media', image['pack']).exists()
        print('PASS bounded HTTP miss queue fetches and verifies requested pack; cache populated')
        # Poisoned cache is repaired from a verified local pack, not served as bytes.
        cache.write_bytes(b'poison')
        assert media(b, path)[1] == description
        b.stop(); a.stop(); b.start()
        assert media(b, path)[1] == description
        assert b.lists()[0]['net']['version_ih'] == original_hash
        assert Path(b.data, 'mesh-local', pk, 'original.torrent').exists()
        assert not b.call('/api/catalog/search?q=unselected')[1]['results']
        print('PASS partial restart preserves original torrent infohash and offline verified cache')
        a.start()
        # Budget refuses mirror admission of the large poster, then admits it after growth.
        assert b.call(f'/api/lists/{pk}/policy', {'mirror_media':True, 'media_bytes':128})[0] == 200
        time.sleep(3)
        assert not Path(b.data, 'lists', pk, 'media', image['pack']).exists()
        assert b.call(f'/api/lists/{pk}/policy', {'mirror_media':True, 'media_bytes':1048576})[0] == 200
        wait(lambda: Path(b.data, 'lists', pk, 'media', image['pack']).exists(), 65)
        image_path = f'/api/catalog/{"a"*40}/poster?list={pk}'
        assert media(b, image_path)[1] == poster
        b.stop(); a.stop(); b.start()
        assert media(b, image_path)[1] == poster
        assert b.call(f'/api/lists/{pk}/policy')[1]['policy']['mirror_media'] is True
        assert b.lists()[0]['net']['version_ih'] == original_hash
        print('PASS full-pack mirror, media budget admission, multi-piece v2 and offline restart')
        a.start()
        assert b.call(f'/api/lists/{pk}/policy', {'shards':['books'], 'mirror_media':False})[0] == 200
        wait(lambda: b.call('/api/catalog/search?q=unselected')[1]['results'], 65)
        assert not b.call('/api/catalog/search?q=selected%20film')[1]['results'] or all(
            r['hash'] != 'a'*40 for r in b.call('/api/catalog/search')[1]['results'])
        assert b.call(f'/api/lists/{pk}/policy', {'shards':['movies']})[0] == 200
        wait(indexed, 65)
        # Snapshot switches immutable identities while subscriber worker is active.
        seq = a.call(f'/api/publish/{pk}/snapshot', {})
        assert seq[0] == 200, seq
        assert b.call(f'/api/lists/{pk}', {'peers':[f'127.0.0.1:{a.bt}']})[0] == 202
        wait(lambda: b.lists()[0]['seq'] == 2 and indexed(), 75)
        assert b.lists()[0]['net']['version_ih'] != original_hash
        assert media(b, path)[1] == description
        print('PASS policy reselection, shard eviction/refetch and live snapshot generation switch')
        # Retire a live partial handle while its HTTP miss is queued. No joined
        # worker snapshot or outstanding response may recreate the removed list.
        assert b.call(f'/api/lists/{pk}/policy', {'media_bytes':0, 'cache_bytes':0})[0] == 200
        wait(lambda: b.lists()[0]['net']['seeding'], 30)
        assert media(b, path)[0] == 503
        remove = urllib.request.Request(f'http://127.0.0.1:{b.http}/api/lists/{pk}', method='DELETE',
                                        headers={'Authorization': 'Bearer ' + TOKEN})
        with urllib.request.urlopen(remove, timeout=10) as response:
            assert response.status == 200
        wait(lambda: not list(network.iterdir()), 30)
        time.sleep(3)
        assert not b.lists() and not Path(b.data, 'lists', pk).exists()
        assert not Path(b.data, 'mesh-local', pk).exists()
        print('PASS removal during queued media work retires storage without resurrection')
        print('PASS selective network tests')
    finally:
        for node in nodes:
            node.stop()
        if os.environ.get('KEEP'):
            print('Logs:', root)
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__':
    main()
