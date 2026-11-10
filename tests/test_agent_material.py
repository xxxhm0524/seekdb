#!/usr/bin/env python3
"""Synthetic validator fixtures only; never submitted as learning evidence."""
import copy
import json
import unittest

from validate_agent_material import MAX_BYTES, MaterialError, pointer, validate_bytes


class MaterialTests(unittest.TestCase):
    def sample(self):
        return {'synthetic': False, 'conversations': [{'role': 'user', 'content': '我的实际问题'}],
            'experiments': [{'role': 'tool', 'execution_status': 'executed', 'output': '实际结果 PASS'}],
            'evidence_index': [{'student_evidence': [{'pointer': '/conversations/0/content', 'quote': '实际问题'}],
                'result_evidence': [{'pointer': '/experiments/0/output', 'quote': '结果 PASS'}]}]}

    def raw(self, value):
        return json.dumps(value, ensure_ascii=False).encode('utf-8')

    def test_valid_references(self):
        result = validate_bytes(self.raw(self.sample()), True)
        self.assertEqual(result['references'], {'student': 1, 'result': 1})
        self.assertFalse(result['learning_score_assigned'])

    def test_format_and_size(self):
        validate_bytes(b'{}'+b' '*(MAX_BYTES-2))
        for raw in (b'{}'+b' '*(MAX_BYTES-1), b'[]', b'null', b'\xff', b'\xef\xbb\xbf{}',
                b'{"x":1,"x":2}', b'{"x":{"z":1,"z":2}}', b'{"x":NaN}', b'{"x":Infinity}', b'{"x":1e9999}'):
            with self.subTest(raw=raw[:30]), self.assertRaises(MaterialError):
                validate_bytes(raw)
        validate_bytes(b'{"output":"NaN is an invalid VECTOR input"}')

    def test_source_and_actual_result(self):
        for change in ('source', 'future_plan', 'quote', 'pointer', 'long_quote', 'duplicate', 'command', 'annotation'):
            value = copy.deepcopy(self.sample())
            if change == 'source': value['conversations'][0]['role'] = 'assistant'
            if change == 'future_plan': value['experiments'][0]['execution_status'] = 'planned'
            if change == 'quote': value['evidence_index'][0]['student_evidence'][0]['quote'] = '不存在'
            if change == 'pointer': value['evidence_index'][0]['student_evidence'][0]['pointer'] = '/missing'
            if change == 'long_quote': value['evidence_index'][0]['student_evidence'][0]['quote'] = '字'*241
            if change == 'duplicate': value['evidence_index'].append(copy.deepcopy(value['evidence_index'][0]))
            if change == 'command':
                value['experiments'][0]['command'] = '将来要跑的命令'
                value['evidence_index'][0]['result_evidence'][0] = {'pointer': '/experiments/0/command', 'quote': '将来要跑'}
            if change == 'annotation':
                value['conversations'][0]['note'] = '助手对学生的转述'
                value['evidence_index'][0]['student_evidence'][0] = {'pointer': '/conversations/0/note', 'quote': '转述'}
            with self.subTest(change=change), self.assertRaises(MaterialError):
                validate_bytes(self.raw(value), True)

    def test_rfc6901_escaping(self):
        value = {'a/b': {'~key': '原文'}}
        self.assertEqual(pointer(value, '/a~1b/~0key')[0], '原文')
        for path in ('a', '/a~2b', '/a~1b/missing'):
            with self.assertRaises(MaterialError): pointer(value, path)

    def test_authenticity_and_credentials(self):
        for value in ({'synthetic': True}, {'instances': [{'score': 0.5}]},
                {'x': 'https://'+'user:test-only@example.com/repo'},
                {'x': 'github_'+'pat_'+'x'*30}, {'password': 'test-fixture-only'},
                {'x': 'password=test-fixture-only'}):
            with self.assertRaises(MaterialError): validate_bytes(self.raw(value))
        validate_bytes(self.raw({'password': '[REDACTED]', 'context': '密码：[REDACTED]'}))


if __name__ == '__main__':
    unittest.main()
