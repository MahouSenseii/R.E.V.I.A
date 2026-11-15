"""STATE-020 helper: how fast can this PC move parameters from RAM onto each graphics card?

Fetching an expert that is not loaded means copying its weights to the card. This times that
copy for blocks of several sizes, from ordinary and from page-locked memory, so a fetch count can
be turned into time for a model of real size.

  python measure_transfer.py --out runs/transfer.json
"""
import argparse, json, time
import torch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='')
    a = ap.parse_args()
    result = {}
    for index in range(torch.cuda.device_count()):
        device = torch.device(f'cuda:{index}')
        name = torch.cuda.get_device_name(device)
        torch.zeros(1, device=device)                       # start the card
        rows = []
        for megabytes in (16, 64, 256, 1024):
            count = megabytes * 2**20 // 2                  # 16-bit weights
            for pinned in (False, True):
                source = torch.empty(count, dtype=torch.float16)
                source.normal_()
                if pinned:
                    source = source.pin_memory()
                best = None
                for _ in range(5):
                    torch.cuda.synchronize(device)
                    started = time.perf_counter()
                    on_card = source.to(device, non_blocking=False)
                    torch.cuda.synchronize(device)
                    elapsed = time.perf_counter() - started
                    best = elapsed if best is None else min(best, elapsed)
                    del on_card
                rows.append(dict(megabytes=megabytes, page_locked=pinned, seconds=best, gigabytes_per_second=megabytes / 1024 / best))
                print(f'{name}: {megabytes:5d} MB {"page-locked" if pinned else "ordinary   "} {1000 * best:8.1f} ms  {megabytes / 1024 / best:6.2f} GB/s', flush=True)
                del source
        result[name] = rows
        torch.cuda.empty_cache()
    if a.out:
        with open(a.out, 'w') as f:
            json.dump(result, f, indent=1)


if __name__ == '__main__':
    main()
