#!/usr/bin/env python3
from __future__ import annotations
import argparse,json,shutil,time
from pathlib import Path


def main()->None:
 p=argparse.ArgumentParser(); p.add_argument('--root',required=True); p.add_argument('--older-than-hours',type=float,default=24)
 p.add_argument('--delete',action='store_true'); p.add_argument('--output'); a=p.parse_args()
 root=Path(a.root); cutoff=time.time()-a.older_than_hours*3600; found=[]
 if root.exists():
  for item in sorted(root.iterdir()):
   if not item.name.startswith('.staging-') or not item.is_dir(): continue
   if item.stat().st_mtime > cutoff: continue
   found.append(str(item))
   if a.delete: shutil.rmtree(item)
 report={'format':'root4duckdb-orphan-cleanup-v1','root':str(root.resolve()),
         'mode':'delete' if a.delete else 'dry-run','orphan_count':len(found),'paths':found}
 if a.output: Path(a.output).write_text(json.dumps(report,indent=2,sort_keys=True)+"\n")
 print(f"[{report['mode'].upper()}] {len(found)} orphan staging directories")

if __name__=='__main__': main()
