#!/usr/bin/env bash
# STATE-021: every run behind the reported numbers. Finished runs are skipped, so it can be stopped and restarted.
#   bash run_all.sh [device] [python]        for example:  bash run_all.sh cuda:0 python
# Run it from this folder. Records go to runs/<set>/<name>-s<seed>.json; afterwards: python summarize.py runs/main
# Keep to one run at a time per graphics card: runs that share a card slow each other down a great deal.
DEVICE="${1:-cuda:0}"; PY="${2:-python}"
run() {   # run <set> <steps> "<seeds>" <name> <arguments for run_find.py ...>
  folder="runs/$1"; steps="$2"; seeds="$3"; name="$4"; shift 4
  mkdir -p "$folder"
  for seed in $seeds; do
    [ -f "$folder/$name-s$seed.json" ] && continue
    "$PY" run_find.py "$@" --seed "$seed" --steps "$steps" --device "$DEVICE" --out "$folder/$name-s$seed.json" > "$folder/$name-s$seed.log" 2>&1
  done
}

# Main comparison: 24,000 steps, seeds 11 to 13.
run main 24000 "11 12 13" guess16     --arm guess
run main 24000 "11 12 13" dense16     --arm dense
run main 24000 "11 12 13" dense4      --arm dense --experts 4
run main 24000 "11 12 13" dense2      --arm dense --experts 2
run main 24000 "11 12 13" fast        --arm fast
run main 24000 "11 12 13" oracle      --arm oracle
run main 24000 "11 12 13" guess32weak --arm guess --guesses 32 --even 0.1

# The tuning runs on the separate seed 1 (12,000 steps, half length). None of these is in a three-seed number.
run pilot 12000 "1" dense16     --arm dense
run pilot 12000 "1" dense2      --arm dense --experts 2
run pilot 12000 "1" oracle      --arm oracle
run pilot 12000 "1" guess16     --arm guess
run pilot 12000 "1" fast        --arm fast
run pilot 12000 "1" sum09       --arm sum --decay 0.9
run pilot 12000 "1" sum08       --arm sum --decay 0.8
run pilot 12000 "1" guess32     --arm guess --guesses 32
run pilot 12000 "1" guess32weak --arm guess --guesses 32 --even 0.1
run pilot 12000 "1" guesswide   --arm guess --wide 0.2
echo "all runs finished"

# The planner alone (about a minute each), as used to choose its settings:
#   python run_find.py --arm guess --planner-only --guesses 16 --even 1      finds all 16 topics
#   python run_find.py --arm guess --planner-only --guesses 16 --even 0      collapses to one guess
#   python run_find.py --arm guess --planner-only --guesses 8  --even 1      merges topics in pairs
#   python run_find.py --arm guess --planner-only --guesses 24 --even 1      settles on 16 of its 24 guesses
# The best any reader could score on each question:
#   python ceiling.py
