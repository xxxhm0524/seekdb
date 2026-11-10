#!/usr/bin/env python3
"""Validate the learning artifact's format, privacy and evidence references.

This does not assign learning scores or infer student understanding from tests.
"""
import argparse
import json
import math
from pathlib import Path
import re

MAX_BYTES = 128*1024


class MaterialError(ValueError):
    pass


def pairs_without_duplicates(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise MaterialError('duplicate JSON key')
        value[key] = item
    return value


def reject_constant(value):
    raise MaterialError('non-standard JSON numeric constant')


def pointer(document, path):
    if not isinstance(path, str) or not path.startswith('/') or re.search(r'~(?![01])', path):
        raise MaterialError('invalid RFC6901 pointer')
    value = document
    ancestors = [document]
    try:
        for part in path[1:].split('/'):
            key = part.replace('~1', '/').replace('~0', '~')
            if isinstance(value, list):
                if not re.fullmatch(r'0|[1-9]\d*', key):
                    raise MaterialError('invalid array index in pointer')
                value = value[int(key)]
            else:
                value = value[key]
            ancestors.append(value)
    except (KeyError, IndexError, TypeError):
        raise MaterialError('evidence pointer does not resolve') from None
    owner = next((item for item in reversed(ancestors[:-1]) if isinstance(item, dict) and
        ('role' in item or 'origin' in item)), {})
    return value, owner


def validate_bytes(raw, require_evidence=False):
    if len(raw) > MAX_BYTES:
        raise MaterialError('material exceeds 128 KiB')
    try:
        document = json.loads(raw.decode('utf-8'), object_pairs_hook=pairs_without_duplicates,
            parse_constant=reject_constant)
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise MaterialError('material must be UTF-8 JSON') from None
    if not isinstance(document, dict):
        raise MaterialError('top-level value must be an object')
    if document.get('synthetic') is True:
        raise MaterialError('synthetic examples cannot be released as authentic learning material')
    patterns = [
        ('GitHub credential', re.compile(r'github_pat_[A-Za-z0-9_]{20,}|gh[pousr]_[A-Za-z0-9]{20,}')),
        ('URL user information', re.compile(r'https?://[^/\s\x27\"<>]*@')),
        ('private key', re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----')),
        ('JWT credential', re.compile(r'\beyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\b')),
        ('unredacted password', re.compile(r'(?:password|密码)\s*[:=：]\s*(?!\[REDACTED\]|\[已脱敏\])[^\s，。；;]+', re.I)),
    ]
    def strings(value):
        if isinstance(value, dict):
            private = {'password', 'pwd', 'authorization', 'security-code', 'token',
                'access_token', 'refresh_token', 'api_key', 'private_key'}
            for key, item in value.items():
                yield key
                if key.lower() in private and isinstance(item, str) and item not in ('', '[REDACTED]', '[已脱敏]'):
                    raise MaterialError('unredacted sensitive field (value omitted)')
                yield from strings(item)
        elif isinstance(value, list):
            for item in value:
                yield from strings(item)
        elif isinstance(value, str):
            yield value
        elif isinstance(value, float) and not math.isfinite(value):
            raise MaterialError('non-finite parsed JSON number')
    for text in strings(document):
        for label, pattern in patterns:
            if pattern.search(text):
                raise MaterialError(label+' pattern in material (value omitted)')
    for instance in document.get('instances', []):
        if isinstance(instance, dict) and 'score' in instance:
            raise MaterialError('remove self-assigned instance scores from the input material')
    seen = set()
    references = {'student': 0, 'result': 0}
    for topic in document.get('evidence_index', []):
        for kind, field in [('student', 'student_evidence'), ('result', 'result_evidence')]:
            for reference in topic.get(field, []):
                quote = reference.get('quote')
                if not isinstance(quote, str) or not 1 <= len(quote) <= 240:
                    raise MaterialError('evidence quote must contain 1 to 240 characters')
                path = reference.get('pointer')
                value, owner = pointer(document, path)
                if not isinstance(value, str) or quote not in value:
                    raise MaterialError('evidence quote does not match its string field')
                role = owner.get('role', owner.get('origin'))
                leaf = path.rsplit('/', 1)[-1].replace('~1', '/').replace('~0', '~')
                if kind == 'student' and role not in ('user', 'student'):
                    raise MaterialError('student evidence points to non-user content')
                if kind == 'student' and leaf not in ('content', 'text', 'message'):
                    raise MaterialError('student evidence must cite the recorded message, not an annotation')
                if kind == 'result' and (role != 'tool' or owner.get('execution_status') != 'executed'):
                    raise MaterialError('result evidence must point to an executed tool result')
                if kind == 'result' and leaf not in ('output', 'actual_result'):
                    raise MaterialError('result evidence must cite actual output, not commands or interpretation')
                if (path, quote) in seen:
                    raise MaterialError('duplicate evidence fragment across topics')
                seen.add((path, quote))
                references[kind] += 1
    if require_evidence and (not references['student'] or not references['result']):
        raise MaterialError('missing student or actual-result references')
    return dict(format_valid=True, bytes=len(raw), references=references,
        learning_score_assigned=False, semantic_deduplication='requires human or evaluator review')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--path', type=Path, default=Path('agent/conversation.json'))
    parser.add_argument('--mirror', type=Path)
    parser.add_argument('--require-evidence', action='store_true')
    parser.add_argument('--source-corpus', type=Path,
        help='Optional local export of independently retrieved visible messages')
    args = parser.parse_args()
    raw = args.path.read_bytes()
    result = validate_bytes(raw, args.require_evidence)
    if args.mirror and args.mirror.read_bytes() != raw:
        raise MaterialError('the compatibility copy differs from the evaluated artifact')
    if args.source_corpus:
        corpus = json.loads(args.source_corpus.read_text(encoding='utf-8'))
        originals = {item['id']: item for item in corpus['records']}
        document = json.loads(raw)
        for item in document.get('conversations', []):
            original = originals.get(item['id'])
            if not original or any(item.get(field) != original.get(field) for field in ('role', 'content')):
                raise MaterialError('message differs from the retrieved visible source')
        result['messages_checked_against_source'] = len(document.get('conversations', []))
    print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()
