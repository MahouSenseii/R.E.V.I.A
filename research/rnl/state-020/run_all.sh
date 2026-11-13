#!/usr/bin/env bash
# STATE-020: every run behind the reported numbers. Finished runs are skipped, so it can be stopped and restarted.
#   bash run_all.sh [device] [python]        for example:  bash run_all.sh cuda:0 python
# Run it from this folder. Records go to runs/<set>/<name>-s<seed>.json; afterwards: python summarize.py runs/main
# Keep to one run at a time per graphics card: runs that share a card slow each other down a great deal.
DEVICE="${1:-cuda:0}"; PY="${2:-python}"
run() {   # run <set> <steps> "<seeds>" <name> <arguments for run_routes.py ...>
  folder="runs/$1"; steps="$2"; seeds="$3"; name="$4"; shift 4
  mkdir -p "$folder"
  for seed in $seeds; do
    [ -f "$folder/$name-s$seed.json" ] && continue
    "$PY" run_routes.py "$@" --seed "$seed" --steps "$steps" --log-every 2000 --device "$DEVICE" --out "$folder/$name-s$seed.json" > "$folder/$name-s$seed.log" 2>&1
  done
}

# Main comparison: 6,000 steps, seeds 11 to 13.
run main 6000 "11 12 13" dense16    --arm dense
run main 6000 "11 12 13" dense8     --arm dense --experts 8
run main 6000 "11 12 13" dense4     --arm dense --experts 4
run main 6000 "11 12 13" dense2     --arm dense --experts 2
run main 6000 "11 12 13" turn       --arm turn --noise 0
run main 6000 "11 12 13" turnfrozen --arm turn --noise 0 --freeze-router
run main 6000 "11 12 13" turnfixed  --arm turn --noise 0 --fixed-routes
run main 6000 "11 12 13" fast       --arm fast --noise 0
run main 6000 "11 12 13" slow16     --arm slow --hold 16 --noise 0
run main 6000 "11 12 13" oracle     --arm oracle

# Trained four times longer: the fair size-for-size comparison.
run long 24000 "11 12 13" turn      --arm turn --noise 0
run long 24000 "11 12 13" dense2    --arm dense --experts 2

# Twice the knowledge: 32 topics, 12,000 steps. (In the study the turn16 run for seed 11 was stopped part-way.)
run scale 12000 "11 12 13" turn32   --arm turn --noise 0 --topics 32 --experts 32
run scale 12000 "11 12 13" turn16   --arm turn --noise 0 --topics 32
run scale 12000 "11 12 13" dense16  --arm dense --topics 32
run scale 12000 "11 12 13" dense2   --arm dense --topics 32 --experts 2

# The tuning runs on the separate seed 1 (6,000 steps). None of these is in a reported three-seed number.
run pilot 6000 "1" dense16          --arm dense
run pilot 6000 "1" dense2           --arm dense --experts 2
run pilot 6000 "1" oracle           --arm oracle
run pilot 6000 "1" turn             --arm turn
run pilot 6000 "1" turn-noise03     --arm turn --noise 0.3
run pilot 6000 "1" turn-noise0      --arm turn --noise 0
run pilot 6000 "1" fast             --arm fast
run pilot 6000 "1" fast-noise0      --arm fast --noise 0
run pilot 6000 "1" slow16           --arm slow --hold 16
run pilot 6000 "1" slow16-noise0    --arm slow --hold 16 --noise 0
run pilot 6000 "1" leaky            --arm leaky --noise 0
echo "all runs finished"
