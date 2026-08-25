#!/usr/bin/env python3
"""Multipart-precondition probe for GCS XML API under GOOG4-HMAC-SHA256.
Question: does GCS enforce x-goog-if-generation-match on CompleteMultipartUpload?"""
import hashlib, hmac, os, re, datetime, urllib.request, urllib.error, urllib.parse

AK = os.environ['GCS_ACCESS_KEY_ID']
SK = os.environ['GCS_SECRET_ACCESS_KEY']
HOST = 'storage.googleapis.com'
BUCKET = 'content-adressable-test-mfilimonov'
KEY = 'ca_manual_check/mp.bin'

def req(method, key, query=None, extra=None, body=b''):
    query = query or {}
    now = datetime.datetime.now(datetime.timezone.utc)
    datestamp = now.strftime('%Y%m%d')
    timestamp = now.strftime('%Y%m%dT%H%M%SZ')
    payload_hash = hashlib.sha256(body).hexdigest()
    headers = {'host': HOST, 'x-goog-content-sha256': payload_hash, 'x-goog-date': timestamp}
    if extra:
        headers.update({k.lower(): v for k, v in extra.items()})
    signed_names = sorted(headers)
    canonical_headers = ''.join(f'{k}:{headers[k]}\n' for k in signed_names)
    signed_headers = ';'.join(signed_names)
    canonical_query = '&'.join(
        f'{urllib.parse.quote(k, safe="")}={urllib.parse.quote(str(v), safe="")}'
        for k, v in sorted(query.items()))
    canonical_uri = f'/{BUCKET}/{key}'
    canonical_request = '\n'.join([method, canonical_uri, canonical_query, canonical_headers,
                                   signed_headers, payload_hash])
    scope = f'{datestamp}/auto/storage/goog4_request'
    string_to_sign = '\n'.join(['GOOG4-HMAC-SHA256', timestamp, scope,
                                hashlib.sha256(canonical_request.encode()).hexdigest()])
    def hm(k, m): return hmac.new(k, m.encode(), hashlib.sha256).digest()
    k = hm(hm(hm(hm(('GOOG4' + SK).encode(), datestamp), 'auto'), 'storage'), 'goog4_request')
    signature = hmac.new(k, string_to_sign.encode(), hashlib.sha256).hexdigest()
    out = dict(headers)
    del out['host']
    out['Authorization'] = (f'GOOG4-HMAC-SHA256 Credential={AK}/{scope}, '
                            f'SignedHeaders={signed_headers}, Signature={signature}')
    url = f'https://{HOST}/{BUCKET}/{key}'
    if canonical_query:
        url += '?' + canonical_query
    r = urllib.request.Request(url, data=body if method in ('PUT', 'POST') else None,
                               headers=out, method=method)
    try:
        with urllib.request.urlopen(r) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()

def show(label, want, got, detail=''):
    print(f'{"OK " if str(want) == str(got) else "XX "}{label}: want {want}, got {got} {detail}')

def do_multipart(precond=None):
    """Run a full multipart upload of 5MiB+1KiB to KEY; return (complete_status, headers, body)."""
    c, h, b = req('POST', KEY, {'uploads': ''})
    assert c == 200, (c, b[:200])
    upload_id = re.search(rb'<UploadId>([^<]+)</UploadId>', b).group(1).decode()
    part1 = b'A' * (5 * 1024 * 1024)
    part2 = b'B' * 1024
    etags = []
    for n, body in ((1, part1), (2, part2)):
        c, h, b = req('PUT', KEY, {'partNumber': n, 'uploadId': upload_id}, body=body)
        assert c == 200, (n, c, b[:200])
        etags.append(h['ETag'])
    xml = '<CompleteMultipartUpload>' + ''.join(
        f'<Part><PartNumber>{n}</PartNumber><ETag>{e}</ETag></Part>'
        for n, e in zip((1, 2), etags)) + '</CompleteMultipartUpload>'
    extra = {'x-goog-if-generation-match': precond} if precond is not None else None
    c, h, b = req('POST', KEY, {'uploadId': upload_id}, extra, xml.encode())
    if c != 200:
        req('DELETE', KEY, {'uploadId': upload_id})  # abort so parts don't linger
    return c, h, b

# cleanup
req('DELETE', KEY)

# 1. multipart complete with x-goog-if-generation-match: 0 on FRESH key
c, h, b = do_multipart('0')
show('1 MP complete if-gen-match:0 fresh', 200, c, b[:120] if c != 200 else '')
gen1 = h.get('x-goog-generation')
print(f'   generation from Complete response header: {gen1}')

# 2. multipart complete with if-gen-match:0 against EXISTING object -> must be 412
c, h, b = do_multipart('0')
show('2 MP complete if-gen-match:0 existing', 412, c, b[:160] if c not in (200, 412) else (b'' if c==412 else b)[:160])

# 3. object intact after refused complete (size must be 5MiB+1KiB, body starts with A)
c, h, b = req('GET', KEY)
size_ok = len(b) == 5 * 1024 * 1024 + 1024 and b[:1] == b'A'
show('3 object intact after 412', True, size_ok, f'(status {c}, len {len(b)})')

# 4. multipart complete with WRONG generation -> 412
c, h, b = do_multipart('12345')
show('4 MP complete wrong gen', 412, c, b[:160] if c not in (412,) else '')

# 5. multipart complete with CORRECT generation -> 200 (conditional overwrite)
c2, h2, b2 = req('HEAD', KEY)
cur_gen = h2.get('x-goog-generation')
c, h, b = do_multipart(cur_gen)
show('5 MP complete correct gen', 200, c, b[:160] if c != 200 else '')
gen2 = h.get('x-goog-generation')
print(f'   new generation: {gen2} (changed: {gen2 != cur_gen})')

# 6. UNCONDITIONAL multipart complete (no precondition) also fine
c, h, b = do_multipart(None)
show('6 MP complete unconditional', 200, c)

# 7. what error code does the 412 carry (for the error-mapping design)?
c, h, b = do_multipart('12345')
m = re.search(rb'<Code>([^<]+)</Code>', b or b'')
print(f'   412 body code: {m.group(1).decode() if m else "(no XML code)"}')

# cleanup
c, h, b = req('DELETE', KEY)
print(f'cleanup delete: {c}')
