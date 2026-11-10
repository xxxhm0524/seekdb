#!/usr/bin/env python3
"""Permutation-invariant TopK across float32 scales and SIMD tail lengths."""
import argparse
import os
import random
import struct
import sys
import tempfile

sys.path.insert(0,os.path.dirname(__file__))
from protocol_client import Client


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seeds',type=int,default=25)
    parser.add_argument('--indexed',action='store_true')
    args = parser.parse_args()
    dimensions = [4,7,8,31,32,127,128]
    def float32(value):
        return struct.unpack('f',struct.pack('f',value))[0]
    with tempfile.TemporaryDirectory(prefix='mvd-precision-') as data:
        client = Client(datadir=data,extra=['--cache-mb=1'])
        def sql(text):
            response = client.request(text)
            assert response.get('ok') is True,(text[:120],response)
            return response
        try:
            for seed in range(args.seeds):
                rng = random.Random(seed)
                dim = dimensions[seed%len(dimensions)]
                vector = [float32(rng.uniform(-1,1)*2**rng.randint(-100,100)) for _ in range(dim)]
                sql('CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(%d))' % dim)
                values = []
                for ident in range(30,0,-1):
                    rng.shuffle(vector)
                    values.append("(%d,'[%s]')" % (ident,','.join('%.9g'%x for x in vector)))
                sql('INSERT INTO t VALUES '+','.join(values))
                if args.indexed: sql('CREATE VECTOR INDEX vi ON t(v)')
                zero = '['+','.join(['0']*dim)+']'
                for suffix,expected in [('LIMIT 10',list(range(1,11))),
                        ('LIMIT 5 OFFSET 3',list(range(4,9))),('DESC LIMIT 3',[1,2,3])]:
                    response = sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,'%s') " % zero+suffix)
                    assert response['rows']==[[i] for i in expected],(seed,dim,suffix,response)
                sql('DROP TABLE t')
        finally:
            client.close()
    print('vector precision model: PASS (%d seeds, dimensions cycle through %s)' % (args.seeds,dimensions))


if __name__=='__main__':
    main()
