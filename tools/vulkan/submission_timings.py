"""Summarize opt-in Vulkan submission/worker timings; never display acceptance."""
import argparse
import csv
import json
import math
from pathlib import Path

NS_FIELDS=("gpu_ns","reuse_wait_ns","pool_reset_ns","retire_ns","record_ns","submit_ns","record_thread_ns")


def summarize(path, first_frame, last_frame):
    if first_frame<0 or last_frame<first_frame:
        raise ValueError("invalid frame window")
    rows=list(csv.DictReader(line for line in Path(path).read_text().splitlines() if line and not line.startswith("#")))
    seen=set(); frames={}; invalid=0
    for row in rows:
        submission=int(row["submission"])
        if submission<=0 or submission in seen:raise ValueError("duplicate/invalid submission")
        seen.add(submission)
        frame=int(row["frame"])
        if not first_frame<=frame<=last_frame:continue
        values={k:float(row[k]) for k in NS_FIELDS}
        if any(not math.isfinite(v) or v<0 for v in values.values()):raise ValueError("invalid duration")
        invalid+=row["gpu_valid"]!="1"
        if row["record_thread_valid"]!="1":raise ValueError("recording crossed a renderer-thread handoff; CPU thread delta unavailable")
        group=frames.setdefault(frame,{"frame":frame,"submissions":0,**{k:0 for k in NS_FIELDS},"upload_bytes":0,"retained_resources":0})
        group["submissions"]+=1
        for k,v in values.items():group[k]+=v
        for k in ("upload_bytes","retained_resources"):
            value=int(row[k])
            if value<0:raise ValueError("invalid counter")
            group[k]+=value
    if set(frames)!=set(range(first_frame,last_frame+1)):raise ValueError("requested window not fully captured")
    if invalid:raise ValueError(f"{invalid} unavailable GPU timestamp results in window")
    def stats(key, divisor):
        ordered=sorted(g[key]/divisor for g in frames.values())
        return {"mean":sum(ordered)/len(ordered),"p50":ordered[math.ceil(.5*len(ordered))-1],
                "p95":ordered[math.ceil(.95*len(ordered))-1],"p99":ordered[math.ceil(.99*len(ordered))-1],"max":ordered[-1]}
    def worst(key):
        return [{(k.removesuffix("_ns")+"_ms" if k in NS_FIELDS else k):(v/1e6 if k in NS_FIELDS else v) for k,v in g.items()}
                for g in sorted(frames.values(),key=lambda g:g[key],reverse=True)[:10]]
    return {"source":str(path),"measurement":"Per-frame sums of GPU submission spans and CPU worker timings; not GPU utilization or display/compositor timing",
            "first_frame":first_frame,"last_frame":last_frame,"frames":len(frames),
            "submissions":sum(g["submissions"] for g in frames.values()),
            "per_frame_ms":{k.removesuffix("_ns"):stats(k,1e6) for k in NS_FIELDS},
            "per_frame_upload_MiB":stats("upload_bytes",1048576),
            "worst_gpu_frames_ms":worst("gpu_ns"),"worst_record_thread_frames_ms":worst("record_thread_ns")}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("trace",type=Path);p.add_argument("--from-frame",type=int,required=True);p.add_argument("--to-frame",type=int,required=True)
    p.add_argument("--output",type=Path);a=p.parse_args()
    result=json.dumps(summarize(a.trace,a.from_frame,a.to_frame),indent=2)+"\n"
    if a.output:a.output.write_text(result)
    print(result,end="")

if __name__=="__main__":main()
