#!/usr/bin/env python3
"""Manual capability probe for GCS XML API with GOOG4-HMAC-SHA256 signing.
Proves whether x-goog-if-generation-match preconditions work with the SAME HMAC pair
that fails to get AWS-style conditionals enforced under sigv4 (x-amz) signing."""
import hashlib, hmac, os, sys, datetime, urllib.request, urllib.error

AK = os.environ['GCS_ACCESS_KEY_ID']
SK = os.environ['GCS_SECRET_ACCESS_KEY']
HOST = 'storage.googleapis.com'
BUCKET = 'content-adressable-test-mfilimonov'
KEY = 'ca_manual_check/goog4.txt'

def sign(method, key, headers_extra=None, body=b''):
    now = datetime.datetime.now(datetime.timezone.utc)
    datestamp = now.strftime('%Y%m%d')
    timestamp = now.strftime('%Y%m%dT%H%M%SZ')
    payload_hash = hashlib.sha256(body).hexdigest()
    headers = {
        'host': HOST,
        'x-goog-content-sha256': payload_hash,
        'x-goog-date': timestamp,
    }
    if headers_extra:
        headers.update({k.lower(): v for k, v in headers_extra.items()})
    signed_names = sorted(headers)
    canonical_headers = ''.join(f'{k}:{headers[k]}\n' for k in signed_names)
    signed_headers = ';'.join(signed_names)
    canonical_uri = f'/{BUCKET}/{key}'
    canonical_request = '\n'.join([method, canonical_uri, '', canonical_headers, signed_headers, payload_hash])
    scope = f'{datestamp}/auto/storage/goog4_request'
    string_to_sign = '\n'.join(['GOOG4-HMAC-SHA256', timestamp, scope,
                                hashlib.sha256(canonical_request.encode()).hexdigest()])
    def hm(k, m): return hmac.new(k, m.encode(), hashlib.sha256).digest()
    k = hm(hm(hm(hm(('GOOG4' + SK).encode(), datestamp), 'auto'), 'storage'), 'goog4_request')
    signature = hmac.new(k, string_to_sign.encode(), hashlib.sha256).hexdigest()
    auth = (f'GOOG4-HMAC-SHA256 Credential={AK}/{scope}, '
            f'SignedHeaders={signed_headers}, Signature={signature}')
    out = dict(headers)
    del out['host']
    out['Authorization'] = auth
    return out

def req(method, key, extra=None, body=b''):
    headers = sign(method, key, extra, body)
    r = urllib.request.Request(f'https://{HOST}/{BUCKET}/{key}', data=body if method in ('PUT','POST') else None,
                               headers=headers, method=method)
    try:
        with urllib.request.urlopen(r) as resp:
            return resp.status, dict(resp.headers), (resp.read() if method == 'GET' else b'')
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()

def show(label, want, got):
    ok = 'OK ' if str(want) == str(got) else 'XX '
    print(f'{ok}{label}: want {want}, got {got}')

# 0. cleanup leftovers
req('DELETE', KEY)
# 1. create-if-absent on fresh key
c, h, b = req('PUT', KEY, {'x-goog-if-generation-match': '0'}, b'v1')
show('1 create-if-absent fresh', 200, c)
gen1 = h.get('x-goog-generation')
print(f'   generation from PUT response: {gen1}')
# 2. create-if-absent again -> 412
c, h, b = req('PUT', KEY, {'x-goog-if-generation-match': '0'}, b'v2')
show('2 create-if-absent existing', 412, c)
# 3. body intact
c, h, b = req('GET', KEY)
show('3 body intact v1', 'v1', b.decode())
# 4. overwrite with WRONG generation -> 412
c, h, b = req('PUT', KEY, {'x-goog-if-generation-match': '12345'}, b'v3')
show('4 overwrite wrong gen', 412, c)
# 5. overwrite with CORRECT generation -> 200 + new generation
c, h, b = req('PUT', KEY, {'x-goog-if-generation-match': gen1}, b'v4')
show('5 overwrite correct gen', 200, c)
gen2 = h.get('x-goog-generation')
print(f'   new generation: {gen2} (changed: {gen2 != gen1})')
# 6. body replaced
c, h, b = req('GET', KEY)
show('6 body replaced v4', 'v4', b.decode())
# 7. HEAD exposes generation
c, h, b = req('HEAD', KEY)
show('7 HEAD generation matches', gen2, h.get('x-goog-generation'))
# 8. DELETE with WRONG generation -> 412 and object still readable
c, h, b = req('DELETE', KEY, {'x-goog-if-generation-match': '12345'})
show('8 delete wrong gen', 412, c)
c, h, b = req('GET', KEY)
show('8b object still readable', 200, c)
# 9. DELETE with CORRECT generation -> 204, then 404
c, h, b = req('DELETE', KEY, {'x-goog-if-generation-match': gen2})
show('9 delete correct gen', 204, c)
c, h, b = req('GET', KEY)
show('9b gone after delete', 404, c)


# ============================================================================
# Part 2 (2026-07-03, run with MODE=multipart): multipart + compose preconditions.
# MEASURED RESULTS on live GCS:
#   - CompleteMultipartUpload SILENTLY IGNORES x-goog-if-generation-match (both
#     if-generation-match:0 against an existing object and a wrong generation
#     returned 200 and overwrote) -> conditional writes must NOT use multipart.
#   - Compose ENFORCES x-goog-if-generation-match (0-on-existing -> 412,
#     wrong gen -> 412, correct gen -> 200) -> the production-grade big-blob
#     path is: multipart (unconditional) to a temp key -> Compose(temp -> final)
#     with the precondition -> delete temp.
#   - The 412 XML body code is literally `PreconditionFailed` (same string AWS
#     uses), so the existing ClickHouse-side detection needs no change.
# The battery itself is committed next to this file: gcs_goog4_mp_probe.py
# (same signer extended with a canonical-query-string parameter).
# ============================================================================
