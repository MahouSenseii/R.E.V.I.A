"""STATE-020: can a model keep the quality of its whole size while loading only the parameters on its route?

The owner's idea for RNL (2026-11-13): parameters should not all be loaded. A small context comes
in, slow signals choose the routes, and only the parts on those routes are needed.

Task. There are `topics` topics. Each has its own table of random facts, (entity, attribute) ->
value; the first `universal` attributes are the same in every topic. A conversation is 16
questions in segments. A segment opens with three cue words of its topic and then asks questions
(two entity syllables, an attribute; the model must give the value). The topic is never repeated
inside a segment, so it has to be carried from the context. The facts are random: the only way to
answer is to hold them in the weights, and how many fit depends on how many weights there are.

Model. A small causal transformer. In every block the feed-forward part is a small shared group
of units that is always on, plus `experts` expert groups:
  dense   every expert is on for every token (an ordinary transformer of that size)
  fast    top-k experts are chosen again at every token (standard mixture-of-experts routing)
  slow    top-k experts are chosen at a clock tick every `hold` tokens and kept until the next
          tick, so the experts a conversation needs can be loaded once and stay loaded
  turn    top-k experts are chosen once per turn by a small planner that reads only the turn's
          opening cue words, so the routes are known before the turn is run
  leaky   top-k experts are chosen at every token, but from a running summary of the words so far
          in which each word fades slowly, so no turn boundaries are needed
  oracle  the route is fixed by the topic and not learned (what a perfect slow router would do)

Reported per run: accuracy on three kinds of conversation (one topic; the topic changes once;
two topics take turns), how many experts a conversation touches, what share of the parameters
that is, and how many experts have to be fetched per token when only a few are kept loaded.

  python run_routes.py --arm slow --hold 16 --seed 1 --device cuda:0 --out runs/pilot/slow16-s1.json
"""
import argparse, json, math, time
from pathlib import Path

import torch
import torch.nn as nn
import torch.nn.functional as F

QUESTIONS = 16
REGIMES = {'one_topic': (16, 'same'), 'one_switch': (8, 'switch'), 'taking_turns': (2, 'turns')}


class Task:
    def __init__(self, a, device):
        self.topics, self.entities, self.attributes, self.universal, self.values, self.cues = a.topics, a.entities, a.attributes, a.universal, a.values, a.cues
        self.S = math.ceil(math.sqrt(a.entities))
        g = torch.Generator().manual_seed(a.data_seed)
        table = torch.randint(a.values, (a.topics, a.entities, a.attributes), generator=g)
        table[:, :, :a.universal] = table[0:1, :, :a.universal]
        self.table = table.to(device)
        self.cue0 = 0
        self.s1_0 = self.cue0 + a.topics * a.cues
        self.s2_0 = self.s1_0 + self.S
        self.a0 = self.s2_0 + self.S
        self.v0 = self.a0 + a.attributes
        self.vocab = self.v0 + a.values
        distinct = a.topics * a.entities * (a.attributes - a.universal) + a.entities * a.universal
        self.distinct_facts, self.bits = distinct, distinct * math.log2(a.values)

    def batch(self, n, per_segment, mode, gen, device):
        """mode: train (each new segment changes topic half the time), same, switch (always changes), turns (two topics alternate)."""
        D, S, g, m = self.topics, self.S, per_segment, QUESTIONS // per_segment
        first = torch.randint(D, (n, 1), generator=gen, device=device)
        if mode == 'same':
            topics = first.expand(n, m)
        else:
            jump = torch.randint(1, D, (n, m), generator=gen, device=device)   # 1..D-1: always a different topic
            if mode == 'train':
                jump = jump * (torch.rand((n, m), generator=gen, device=device) < 0.5)
            if mode == 'turns':
                other = (first + jump[:, :1]) % D
                topics = torch.where((torch.arange(m, device=device) % 2 == 0)[None, :], first, other)
            else:
                jump[:, 0] = 0
                topics = (first + jump.cumsum(1)) % D
        cue = self.cue0 + topics[:, :, None] * self.cues + torch.randint(self.cues, (n, m, 3), generator=gen, device=device)
        ent = torch.randint(self.entities, (n, m, g), generator=gen, device=device)
        att = torch.randint(self.attributes, (n, m, g), generator=gen, device=device)
        val = self.table[topics[:, :, None].expand(n, m, g), ent, att]
        q = torch.stack([self.s1_0 + ent // S, self.s2_0 + ent % S, self.a0 + att, self.v0 + val], dim=-1)

        def lay(cue_part, question_part):   # (n, m, 3) and (n, m, g, 4) -> (n, T)
            return torch.cat([cue_part, question_part.reshape(n, m, 4 * g)], dim=-1).reshape(n, m * (3 + 4 * g))

        minus = lambda *shape: torch.full(shape, -1, device=device, dtype=torch.long)
        target = minus(n, m, g, 4); target[..., 2] = val
        number = minus(n, m, g, 4); number[..., 2] = torch.arange(g, device=device)[None, None, :]
        shared = minus(n, m, g, 4); shared[..., 2] = (att < self.universal).long()
        kind = torch.arange(1, 5, device=device).expand(n, m, g, 4)
        seg = torch.arange(m, device=device)[None, :, None].expand(n, m, 3 + 4 * g).reshape(n, -1)
        return dict(tokens=lay(cue, q), target=lay(minus(n, m, 3), target), number=lay(minus(n, m, 3), number), shared=lay(minus(n, m, 3), shared),
                    kind=lay(torch.zeros((n, m, 3), device=device, dtype=torch.long), kind), segment=seg,
                    topic=topics[:, :, None].expand(n, m, 3 + 4 * g).reshape(n, -1))


class Block(nn.Module):
    def __init__(self, a):
        super().__init__()
        self.a, d = a, a.width
        self.ln1, self.qkv, self.proj = nn.LayerNorm(d), nn.Linear(d, 3 * d), nn.Linear(d, d)
        self.ln2 = nn.LayerNorm(d)
        self.w1, self.w2 = nn.Linear(d, a.shared + a.experts * a.expert), nn.Linear(a.shared + a.experts * a.expert, d)
        self.router = nn.Linear(d, a.experts) if a.arm in ('fast', 'slow', 'turn', 'leaky') else None

    def route(self, h, topic, hold, noise, context=None):
        """Returns (gates (B,T,N) or None, chosen (B,T,N) bool or None, balance term)."""
        a, N, k = self.a, self.a.experts, self.a.active
        if a.arm == 'dense':
            return None, None, h.new_zeros(())
        if a.arm == 'oracle':
            index = (topic[..., None] * k + torch.arange(k, device=h.device)) % N
            chosen = torch.zeros(*topic.shape, N, device=h.device, dtype=torch.bool).scatter(-1, index, True)
            return chosen.to(h.dtype), chosen, h.new_zeros(())
        # Decisions are made per token (fast), at clock ticks (slow), or once per turn from its summary (turn).
        logits = self.router(h if context is None else context)
        gumbel = -torch.log(-torch.log(torch.rand_like(logits).clamp(1e-9, 1 - 1e-9))) if noise > 0 else None
        if hold is not None:
            held = hold[..., None].expand(-1, -1, N)
            logits = logits.gather(1, held)             # the decision in force at each token
            if gumbel is not None:
                gumbel = gumbel.gather(1, held)         # one draw per decision, or the route would flicker inside a hold
        scores = logits if gumbel is None else logits + noise * gumbel
        top = scores.topk(k, dim=-1).indices
        chosen = torch.zeros_like(logits, dtype=torch.bool).scatter(-1, top, True)
        gates = torch.zeros_like(logits).scatter(-1, top, F.softmax(logits.gather(-1, top), dim=-1))
        # Busy experts are given less probability: N * sum_i (share of routes)_i * (mean probability)_i.
        share = chosen.float().mean((0, 1)) / k
        balance = N * (share.detach() * F.softmax(logits, dim=-1).mean((0, 1))).sum()
        return gates, chosen, balance

    def forward(self, x, topic, hold, noise, context=None):
        a = self.a
        B, T, d = x.shape
        q, k, v = self.qkv(self.ln1(x)).view(B, T, 3, a.heads, d // a.heads).permute(2, 0, 3, 1, 4)
        x = x + self.proj(F.scaled_dot_product_attention(q, k, v, is_causal=True).transpose(1, 2).reshape(B, T, d))
        h = self.ln2(x)                                  # the router reads the state after attention, so it can see the context
        gates, chosen, balance = self.route(h, topic, hold, noise, context)
        u = F.relu(self.w1(h))
        if gates is not None:
            u = u * torch.cat([u.new_ones(B, T, a.shared), gates.repeat_interleave(a.expert, dim=-1)], dim=-1)
        return x + self.w2(u), chosen, balance


class Net(nn.Module):
    def __init__(self, a, task):
        super().__init__()
        self.a = a
        self.tok, self.pos = nn.Embedding(task.vocab, a.width), nn.Embedding(96, a.width)
        nn.init.normal_(self.tok.weight, std=0.1); nn.init.normal_(self.pos.weight, std=0.1)
        self.blocks = nn.ModuleList(Block(a) for _ in range(a.blocks))
        self.ln, self.head = nn.LayerNorm(a.width), nn.Linear(a.width, task.values)
        self.plan = nn.LayerNorm(a.width) if a.arm in ('turn', 'leaky') else None
        # --fixed-routes: the planner reads its own table of word vectors, random and never trained, so the route is
        # a fixed random function of the opening words and nothing about it is learned.
        self.key = nn.Embedding(task.vocab, a.width) if getattr(a, 'fixed_routes', False) else None
        if self.key is not None:
            nn.init.normal_(self.key.weight, std=0.1)

    def forward(self, tokens, topic, phase=None, noise=0.0, segment=None, kind=None):
        B, T = tokens.shape
        words = self.tok(tokens)
        x = words + self.pos(torch.arange(T, device=tokens.device))
        hold, context = None, None
        if self.a.arm == 'slow':
            t, p = torch.arange(T, device=tokens.device)[None, :], phase[:, None]
            hold = torch.where(t < p, torch.zeros_like(t), p + ((t - p) // self.a.hold) * self.a.hold)   # position of the last tick
        if self.a.arm == 'turn':
            # A small planner reads the opening words of each turn (their mean embedding, nothing else) and every
            # block picks its experts for the whole turn from that. The routes are known before the turn is run.
            d = words.shape[-1]
            opening = torch.zeros(B, int(segment.max()) + 1, d, device=words.device, dtype=words.dtype)
            seen = words if self.key is None else self.key(tokens)
            opening.scatter_add_(1, segment[..., None].expand(-1, -1, d), seen * (kind == 0)[..., None])
            context, hold = self.plan(opening / 3), segment
        if self.a.arm == 'leaky':
            # No turn boundaries are given. The planner keeps a running summary of the words so far, in which each
            # word fades by `decay` per token, and every block picks its experts from that summary at every token.
            # The route only moves when the summary does.
            t = torch.arange(T, device=tokens.device)
            fade = torch.tril(self.a.decay ** (t[:, None] - t[None, :]).clamp(min=0).to(words.dtype))
            context = self.plan(fade @ words)
        balance, routes = x.new_zeros(()), []
        for block in self.blocks:
            x, chosen, b = block(x, topic, hold, noise, context)
            balance = balance + b; routes.append(chosen)
        return self.head(self.ln(x)), balance, routes


def sizes(a, model):
    total = sum(p.numel() for p in model.parameters())
    per_expert = a.expert * (2 * a.width + 1)
    always = total - a.blocks * a.experts * per_expert
    active = a.experts if a.arm == 'dense' else a.active
    return dict(total=total, always_loaded=always, per_expert=per_expert, loaded_per_token=always + a.blocks * active * per_expert)


def fetches(chosen, kept):
    """chosen: (T, N) bool for one conversation and block. Experts fetched after the first token when `kept` stay loaded (least recently used leaves)."""
    cache, count = [], 0
    for t, row in enumerate(chosen):
        for e in row.nonzero()[0]:
            e = int(e)
            if e in cache:
                cache.remove(e)
            else:
                count += t > 0
                if len(cache) >= kept:
                    cache.pop()
            cache.insert(0, e)
    return count


@torch.no_grad()
def evaluate(model, task, a, regime, device, conversations=2048, with_routes=True):
    model.eval()
    per_segment, mode = REGIMES[regime]
    gen = torch.Generator(device=device).manual_seed(424200 + list(REGIMES).index(regime))
    N, k = a.experts, a.active
    right = total = 0
    by = {}
    touched, changes, loaded = [], [], []
    usage_topic = torch.zeros(a.blocks, a.topics, N, device=device); count_topic = torch.zeros(a.topics, device=device)
    usage_kind = torch.zeros(a.blocks, 5, N, device=device); count_kind = torch.zeros(5, device=device)
    fetch = {m: 0 for m in sorted({min(N, k), min(N, 2 * k), min(N, 4 * k)})}
    fetch_tokens = 0
    size = sizes(a, model)
    for _ in range(conversations // 256):
        b = task.batch(256, per_segment, mode, gen, device)
        phase = torch.randint(a.hold, (256,), generator=gen, device=device) if a.arm == 'slow' else None
        logits, _, routes = model(b['tokens'], b['topic'], phase, 0.0, b['segment'], b['kind'])
        asked = b['target'] >= 0
        ok = (logits.argmax(-1) == b['target']) & asked
        right += int(ok.sum()); total += int(asked.sum())
        for name, mask in (('universal', asked & (b['shared'] == 1)), ('topical', asked & (b['shared'] == 0)),
                           ('first_segment', asked & (b['segment'] == 0)), ('later_segments', asked & (b['segment'] > 0))):
            r, n = by.get(name, (0, 0)); by[name] = (r + int((ok & mask).sum()), n + int(mask.sum()))
        for q in range(per_segment):   # how soon after a change of topic the answers recover
            mask = asked & (b['segment'] > 0) & (b['number'] == q)
            r, n = by.get(f'later_q{q}', (0, 0)); by[f'later_q{q}'] = (r + int((ok & mask).sum()), n + int(mask.sum()))
        if not with_routes or routes[0] is None:
            continue
        per_block = torch.stack([c.any(1).sum(-1).float() for c in routes], dim=0)            # (blocks, B)
        touched.append(per_block.mean(0)); loaded.append(size['always_loaded'] + per_block.sum(0) * size['per_expert'])
        changes.append(torch.stack([(c[:, 1:] != c[:, :-1]).any(-1).float().mean(1) for c in routes], dim=0).mean(0))
        for i, c in enumerate(routes):
            usage_topic[i].index_add_(0, b['topic'].reshape(-1), c.reshape(-1, N).float())
            usage_kind[i].index_add_(0, b['kind'].reshape(-1), c.reshape(-1, N).float())
        count_topic += torch.bincount(b['topic'].reshape(-1), minlength=a.topics); count_kind += torch.bincount(b['kind'].reshape(-1), minlength=5)
        if fetch_tokens == 0:   # the first 64 conversations are enough for the paging count
            cpu = [c[:64].cpu().numpy() for c in routes]
            for c in cpu:
                for conversation in c:
                    for m in fetch:
                        fetch[m] += fetches(conversation, m)
            fetch_tokens = 64 * (b['tokens'].shape[1] - 1)
    out = dict(accuracy=right / total, questions=total, **{name: (r / n if n else None) for name, (r, n) in by.items()})
    if touched:
        ut, uk = usage_topic / count_topic[None, :, None], usage_kind / count_kind[None, :, None]
        out.update(
            experts_touched_per_block=float(torch.cat(touched).mean()),
            parameters_loaded=float(torch.cat(loaded).mean()), share_of_parameters_loaded=float(torch.cat(loaded).mean()) / size['total'],
            route_changes_per_token=float(torch.cat(changes).mean()),
            # 1.0: a topic (or a kind of token) always uses the same k experts. k/N: it uses all of them equally.
            route_follows_topic=float(ut.topk(k, dim=-1).values.sum(-1).mean() / k), route_follows_token_kind=float(uk.topk(k, dim=-1).values.sum(-1).mean() / k),
            experts_fetched_per_token={f'keeping_{m}_per_block': n / fetch_tokens for m, n in fetch.items()})
    model.train()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arm', required=True, choices=['dense', 'fast', 'slow', 'turn', 'leaky', 'oracle'])
    ap.add_argument('--out', default='')
    ap.add_argument('--device', default='cuda:0')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--data-seed', type=int, default=20261113)
    for name, value in dict(topics=16, entities=2048, attributes=8, universal=2, values=64, cues=4, width=128, heads=4, blocks=3, shared=64,
                            expert=128, experts=16, active=2, hold=16, steps=12000, batch=256, warmup=300, log_every=1000).items():
        ap.add_argument('--' + name.replace('_', '-'), type=int, default=value)
    ap.add_argument('--lr', type=float, default=1e-3)
    ap.add_argument('--balance', type=float, default=0.01)
    ap.add_argument('--noise', type=float, default=1.0)
    ap.add_argument('--decay', type=float, default=0.98)
    ap.add_argument('--freeze-router', action='store_true', help="the router's own weights are never trained (the word vectors it reads still are)")
    ap.add_argument('--fixed-routes', action='store_true', help='turn arm only: the route is a fixed random function of the opening words; nothing about it is trained')
    a = ap.parse_args()
    device = torch.device(a.device)
    torch.manual_seed(a.seed)
    task = Task(a, device)
    model = Net(a, task).to(device)
    size = sizes(a, model)
    if a.freeze_router or a.fixed_routes:
        for block in model.blocks:
            block.router.requires_grad_(False)
    if a.fixed_routes:
        assert a.arm == 'turn'
        model.key.requires_grad_(False); model.plan.requires_grad_(False)
    opt = torch.optim.AdamW([p for p in model.parameters() if p.requires_grad], lr=a.lr, weight_decay=0.0)
    gen = torch.Generator(device=device).manual_seed(1000 + a.seed)
    learned = a.arm in ('fast', 'slow', 'turn', 'leaky')
    print(f'arm={a.arm} hold={a.hold if a.arm == "slow" else "-"} seed={a.seed} parameters={size["total"]} loaded_per_token={size["loaded_per_token"]} '
          f'facts={task.distinct_facts} ({task.bits / 1e6:.2f} Mbit) device={torch.cuda.get_device_name(device)}', flush=True)
    curve, started, loss_sum, seen = [], time.perf_counter(), 0.0, 0
    for step in range(1, a.steps + 1):
        rate = a.lr * (step / a.warmup if step <= a.warmup else 0.05 + 0.95 * 0.5 * (1 + math.cos(math.pi * (step - a.warmup) / max(1, a.steps - a.warmup))))
        for group in opt.param_groups:
            group['lr'] = rate
        noise = a.noise * max(0.0, 1.0 - (step - 1) / a.steps / 0.5) if learned else 0.0
        b = task.batch(a.batch, (16, 8, 4, 2)[int(torch.randint(4, (1,), generator=gen, device=device))], 'train', gen, device)
        phase = torch.randint(a.hold, (a.batch,), generator=gen, device=device) if a.arm == 'slow' else None
        logits, balance, _ = model(b['tokens'], b['topic'], phase, noise, b['segment'], b['kind'])
        answer = F.cross_entropy(logits.reshape(-1, task.values), b['target'].reshape(-1), ignore_index=-1)
        loss = answer + a.balance * balance
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        loss_sum += answer.item(); seen += 1
        if step % a.log_every == 0 or step == a.steps:
            quick = evaluate(model, task, a, 'one_topic', device, conversations=512, with_routes=False)
            seconds = time.perf_counter() - started
            curve.append(dict(step=step, loss=loss_sum / seen, one_topic=quick['accuracy'], seconds=round(seconds, 1)))
            print(f'step {step} loss {loss_sum / seen:.4f} one-topic accuracy {quick["accuracy"]:.4f} | {seconds:.0f} s, {step / seconds:.1f} steps/s', flush=True)
            loss_sum, seen = 0.0, 0
    seconds = time.perf_counter() - started
    result = dict(arm=a.arm, hold=a.hold if a.arm == 'slow' else None, seed=a.seed, config=vars(a), parameters=size,
                  facts=dict(distinct=task.distinct_facts, bits=task.bits), seconds=round(seconds, 1), torch=torch.__version__,
                  device=torch.cuda.get_device_name(device), curve=curve, regimes={name: evaluate(model, task, a, name, device) for name in REGIMES})
    for name, r in result['regimes'].items():
        extra = '' if 'share_of_parameters_loaded' not in r else f' | experts touched {r["experts_touched_per_block"]:.2f} per block, {100 * r["share_of_parameters_loaded"]:.1f}% of parameters'
        print(f'{name}: accuracy {r["accuracy"]:.4f}{extra}', flush=True)
    if a.out:
        Path(a.out).parent.mkdir(parents=True, exist_ok=True)
        Path(a.out).write_text(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
