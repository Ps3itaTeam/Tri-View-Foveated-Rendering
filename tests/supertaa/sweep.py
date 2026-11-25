"""Runs variants of the resolvers over the synthetic sequences and prints the measures against the exact image.
   sweep.py <spec.json>   (the sequences: python3 scene.py data)"""
import sys, json, numpy as np
from multiprocessing import Pool
import evaluate as E, resolvers as R

import os

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")

def one(job):
    name, kind, phases, cls, kw = job
    seq = dict(np.load(f"{DATA}/{kind}_p{phases}.npz"))
    make = (lambda: R.OldTAA(**kw)) if cls == "old" else (lambda: R.NewTAA(**kw))
    outs = E.run(seq, make)
    return name, kind, E.metrics(seq, outs)

def sweep(variants, kinds):
    jobs = [(n, k, p, c, kw) for k in kinds for n, (p, c, kw) in variants.items()]
    with Pool(2) as pool:
        results = pool.map(one, jobs)
    for kind in kinds:
        res = {n: m for (n, k, m) in results if k == kind}
        res = {n: res[n] for n in variants}
        print(f"===== {kind} =====")
        print(E.summary(res), flush=True)

if __name__ == "__main__":
    spec = json.load(open(sys.argv[1]))
    sweep({k: tuple(v) for k, v in spec["variants"].items()}, spec["kinds"])
