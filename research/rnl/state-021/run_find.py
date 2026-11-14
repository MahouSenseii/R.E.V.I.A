"""STATE-021: can a routed model find the topic by itself?

STATE-020 showed that a model can answer with a quarter of its parameters loaded if its route is
planned once per turn. But it was helped twice: every turn opened with cue words that named the
topic, and the planner was told where each turn starts. Both routers that had to find the topic
themselves failed. This study takes the help away.

Task. `topics` topics. Names are two syllables. There are `first` first syllables and each topic
uses `own` of them, chosen at random, so a syllable may belong to one topic, to several, or to
none: one name narrows the topic down, a few names settle it. A conversation is 16 questions
(first syllable, second syllable, attribute -> value) and nothing else: no cue words, no markers.
The same name has different values in different topics, so the topic has to be worked out from
the names asked about so far, and it can change without notice.

Model. As in STATE-020: a small causal transformer whose feed-forward part is, per block, a small
shared group that is always on plus `experts` expert groups, `active` of them in use.
  dense   every expert is on for every token (an ordinary transformer of that size)
  oracle  the route is a fixed function of the true topic (what a perfect topic finder would give)
  fast    experts are chosen again at every token from the model's own state (mixture of experts)
  sum     experts are chosen at every token from a running average of the words so far in which
          each word fades by `decay` per token; trained only through the answers
  guess   a small separate planner reads the words and keeps a guess, one of `guesses` numbers,
          about what is being talked about. It is trained beforehand, without topic labels and
          without the answers, only to predict which words come next. The route is a fixed
          function of its guess, so it could be run before any expert is loaded.

  python run_find.py --arm guess --seed 1 --device cuda:0 --out runs/main/guess16-s11.json
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
        self.a, self.values = a, a.values
        g = torch.Generator().manual_seed(a.data_seed)
        self.own = torch.stack([torch.randperm(a.first, generator=g)[:a.own] for _ in range(a.topics)]).to(device)     # (topics, own)
        self.table = torch.randint(a.values, (a.topics, a.first * a.second, a.attributes), generator=g).to(device)
        self.s1_0, self.s2_0 = 0, a.first
        self.a0 = self.s2_0 + a.second
        self.v0 = self.a0 + a.attributes
        self.vocab = self.v0 + a.values
        self.distinct_facts = a.topics * a.own * a.second * a.attributes
        self.bits = self.distinct_facts * math.log2(a.values)
        owners = torch.zeros(a.first)
        for t in range(a.topics):
            owners[self.own[t].cpu()] += 1
        # How many topics a name's first syllable fits, averaged over the names that are asked.
        self.topics_per_name = float((owners * owners).sum() / owners.sum())
        self.owners = owners.long().to(device)
        # The best any reader can do on the very first question, where the name is the only clue: pick one of the
        # topics the name fits (right 1 time in that many), and a wrong pick still hits the value 1 time in `values`.
        chance = 1.0 / owners.clamp(min=1)
        self.best_first_question = float((owners * (chance + (1 - chance) / a.values)).sum() / owners.sum())
        self.names_with_one_topic = float(owners[owners == 1].sum() / owners.sum())

    def batch(self, n, per_segment, mode, gen, device):
        """mode: train (each new segment changes topic half the time), same, switch (always changes), turns (two topics alternate)."""
        a, D, g, m = self.a, self.a.topics, per_segment, QUESTIONS // per_segment
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
        tt = topics[:, :, None].expand(n, m, g)
        s1 = self.own[tt, torch.randint(a.own, (n, m, g), generator=gen, device=device)]
        s2 = torch.randint(a.second, (n, m, g), generator=gen, device=device)
        att = torch.randint(a.attributes, (n, m, g), generator=gen, device=device)
        val = self.table[tt, s1 * a.second + s2, att]
        q = torch.stack([self.s1_0 + s1, self.s2_0 + s2, self.a0 + att, self.v0 + val], dim=-1)                   # (n, m, g, 4)
        flat = lambda x: x.reshape(n, -1)
        minus = lambda: torch.full((n, m, g, 4), -1, device=device, dtype=torch.long)
        target = minus(); target[..., 2] = val
        number = minus(); number[..., 2] = torch.arange(g, device=device)[None, None, :]
        return dict(tokens=flat(q), target=flat(target), number=flat(number),
                    kind=flat(torch.arange(4, device=device).expand(n, m, g, 4)),
                    segment=flat(torch.arange(m, device=device)[None, :, None, None].expand(n, m, g, 4)),
                    topic=flat(tt[..., None].expand(n, m, g, 4)))


def fixed_pairs(keys, experts, active, gen):
    """For each key (a topic or a guess) the experts it always uses in one block. Every expert serves the same
    number of keys when that divides evenly, and no key gets the same expert twice."""
    while True:
        pairs = torch.stack([torch.randperm(keys, generator=gen) % experts for _ in range(active)], dim=-1)
        if all(len(set(row.tolist())) == active for row in pairs):
            return pairs


class Planner(nn.Module):
    """Reads the words and keeps a guess (one of `guesses` numbers) about what is being talked about.
    It learns by predicting the next `window` words through that guess alone: each guess has its own
    list of how likely every word is. No topic labels and no answers are used."""

    def __init__(self, vocab, width, guesses):
        super().__init__()
        self.emb, self.gru = nn.Embedding(vocab, width), nn.GRU(width, width, batch_first=True)
        self.to_guess = nn.Linear(width, guesses)
        self.words = nn.Parameter(0.01 * torch.randn(guesses, vocab))

    def forward(self, tokens):
        return self.to_guess(self.gru(self.emb(tokens))[0])                                   # (B, T, guesses)

    def loss(self, tokens, window):
        logz = F.log_softmax(self(tokens), dim=-1)
        likely = F.log_softmax(self.words, dim=-1)[:, tokens].permute(1, 2, 0)                # (B, T, guesses): log P(word at t | guess)
        T, total = tokens.shape[1], 0.0
        for j in range(1, window + 1):
            total = total + torch.logsumexp(logz[:, :T - j] + likely[:, j:], dim=-1).mean()
        usage = logz.exp().mean((0, 1))
        even = (usage * (usage.clamp_min(1e-9).log() + math.log(usage.numel()))).sum()        # 0 when every guess is used equally often
        return -total / window, even


class Block(nn.Module):
    def __init__(self, a, pairs):
        super().__init__()
        self.a, d = a, a.width
        self.ln1, self.qkv, self.proj = nn.LayerNorm(d), nn.Linear(d, 3 * d), nn.Linear(d, d)
        self.ln2 = nn.LayerNorm(d)
        self.w1, self.w2 = nn.Linear(d, a.shared + a.experts * a.expert), nn.Linear(a.shared + a.experts * a.expert, d)
        self.router = nn.Linear(d, a.experts) if a.arm in ('fast', 'sum') else None
        self.register_buffer('pairs', pairs)

    def route(self, h, key, context):
        """Returns (gates (B,T,N) or None, chosen (B,T,N) bool or None, balance term)."""
        a, N, k = self.a, self.a.experts, self.a.active
        if a.arm == 'dense':
            return None, None, h.new_zeros(())
        if a.arm in ('oracle', 'guess'):
            chosen = torch.zeros(*key.shape, N, device=h.device, dtype=torch.bool).scatter(-1, self.pairs[key], True)
            if context is not None:        # the planner is unsure: the experts of its second guess are switched on as well
                second, unsure = context
                chosen = chosen | (torch.zeros_like(chosen).scatter(-1, self.pairs[second], True) & unsure[..., None])
            return chosen.to(h.dtype), chosen, h.new_zeros(())
        logits = self.router(h if context is None else context)
        top = logits.topk(k, dim=-1).indices
        chosen = torch.zeros_like(logits, dtype=torch.bool).scatter(-1, top, True)
        gates = torch.zeros_like(logits).scatter(-1, top, F.softmax(logits.gather(-1, top), dim=-1))
        share = chosen.float().mean((0, 1)) / k
        balance = N * (share.detach() * F.softmax(logits, dim=-1).mean((0, 1))).sum()      # busy experts are given less probability
        return gates, chosen, balance

    def forward(self, x, key, context):
        a = self.a
        B, T, d = x.shape
        q, k, v = self.qkv(self.ln1(x)).view(B, T, 3, a.heads, d // a.heads).permute(2, 0, 3, 1, 4)
        x = x + self.proj(F.scaled_dot_product_attention(q, k, v, is_causal=True).transpose(1, 2).reshape(B, T, d))
        h = self.ln2(x)
        gates, chosen, balance = self.route(h, key, context)
        u = F.relu(self.w1(h))
        if gates is not None:
            u = u * torch.cat([u.new_ones(B, T, a.shared), gates.repeat_interleave(a.expert, dim=-1)], dim=-1)
        return x + self.w2(u), chosen, balance


class Net(nn.Module):
    def __init__(self, a, task):
        super().__init__()
        self.a = a
        self.tok, self.pos = nn.Embedding(task.vocab, a.width), nn.Embedding(4 * QUESTIONS, a.width)
        nn.init.normal_(self.tok.weight, std=0.1); nn.init.normal_(self.pos.weight, std=0.1)
        gen = torch.Generator().manual_seed(a.data_seed + 7)
        keys = a.guesses if a.arm == 'guess' else a.topics
        self.blocks = nn.ModuleList(Block(a, fixed_pairs(keys, a.experts, a.active, gen)) for _ in range(a.blocks))
        self.ln, self.head = nn.LayerNorm(a.width), nn.Linear(a.width, task.values)
        self.planner = Planner(task.vocab, a.planner_width, a.guesses) if a.arm == 'guess' else None

    def forward(self, tokens, topic):
        B, T = tokens.shape
        words = self.tok(tokens)
        x = words + self.pos(torch.arange(T, device=tokens.device))
        key, context = topic, None
        if self.a.arm == 'guess':
            with torch.no_grad():
                top = self.planner(tokens).softmax(-1).topk(2, dim=-1)
                key = top.indices[..., 0]                                 # the planner's guess after reading each word
                if self.a.wide > 0:                                       # and its second guess, while that still has a real chance
                    context = (top.indices[..., 1], top.values[..., 1] >= self.a.wide)
        if self.a.arm == 'sum':
            t = torch.arange(T, device=tokens.device)
            fade = torch.tril(self.a.decay ** (t[:, None] - t[None, :]).clamp(min=0).to(words.dtype))
            context = (fade @ words) / fade.sum(-1, keepdim=True)         # a running average in which each word fades
        balance, routes = x.new_zeros(()), []
        for block in self.blocks:
            x, chosen, b = block(x, key, context)
            balance = balance + b; routes.append(chosen)
        return self.head(self.ln(x)), balance, routes, (key if self.a.arm == 'guess' else None)


def sizes(a, model):
    total = sum(p.numel() for p in model.parameters())
    per_expert = a.expert * (2 * a.width + 1)
    always = total - a.blocks * a.experts * per_expert
    active = a.experts if a.arm == 'dense' else a.active
    return dict(total=total, always_loaded=always, per_expert=per_expert, loaded_per_token=always + a.blocks * active * per_expert,
                planner=sum(p.numel() for p in model.planner.parameters()) if model.planner is not None else 0)


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
def guess_to_topic(model, task, a, device):
    """Which topic each guess stands for: the topic it is most often given to, on fresh one-topic conversations."""
    gen = torch.Generator(device=device).manual_seed(515100)
    counts = torch.zeros(a.guesses, a.topics, device=device)
    for _ in range(8):
        b = task.batch(256, 16, 'same', gen, device)
        guess = model.planner(b['tokens']).argmax(-1)
        counts.index_put_((guess.reshape(-1), b['topic'].reshape(-1)), torch.ones(guess.numel(), device=device), accumulate=True)
    return counts.argmax(-1), counts


@torch.no_grad()
def evaluate(model, task, a, regime, device, conversations=2048, with_routes=True):
    model.eval()
    per_segment, mode = REGIMES[regime]
    gen = torch.Generator(device=device).manual_seed(424200 + list(REGIMES).index(regime))
    N, k = a.experts, a.active
    right = total = 0
    by = {}
    touched, changes, loaded, in_use = [], [], [], []
    usage_topic = torch.zeros(a.blocks, a.topics, N, device=device); count_topic = torch.zeros(a.topics, device=device)
    usage_kind = torch.zeros(a.blocks, 4, N, device=device); count_kind = torch.zeros(4, device=device)
    fetch = {m: 0 for m in sorted({min(N, k), min(N, 2 * k), min(N, 4 * k)})}
    fetch_tokens = 0
    size = sizes(a, model)
    stands_for = guess_to_topic(model, task, a, device)[0] if a.arm == 'guess' else None

    sure = {}

    def add(name, good, mask):
        r, n = by.get(name, (0, 0)); by[name] = (r + int((good & mask).sum()), n + int(mask.sum()))

    for _ in range(conversations // 256):
        b = task.batch(256, per_segment, mode, gen, device)
        logits, _, routes, guess = model(b['tokens'], b['topic'])
        asked = b['target'] >= 0
        ok = (logits.argmax(-1) == b['target']) & asked
        right += int(ok.sum()); total += int(asked.sum())
        add('first_segment', ok, asked & (b['segment'] == 0)); add('later_segments', ok, asked & (b['segment'] > 0))
        # The very first question, split by how many topics its name fits, with how sure the model says it is.
        fits = task.owners[b['tokens'].roll(2, dims=1).clamp(max=a.first - 1)]      # at an answer position, the name began two tokens back
        opening = asked & (b['segment'] == 0) & (b['number'] == 0)
        confidence = logits.softmax(-1).max(-1).values
        for name, mask in (('fits_1_topic', fits == 1), ('fits_2_topics', fits == 2), ('fits_3_or_more', fits >= 3)):
            add(f'opening_question_{name}', ok, opening & mask)
            c, n = sure.get(name, (0.0, 0)); sure[name] = (c + float(confidence[opening & mask].sum()), n + int((opening & mask).sum()))
        for q in range(per_segment):   # how many questions it takes to find the topic, at the start and after a change
            add(f'first_q{q}', ok, asked & (b['segment'] == 0) & (b['number'] == q))
            add(f'later_q{q}', ok, asked & (b['segment'] > 0) & (b['number'] == q))
        if guess is not None:          # is the planner's guess the right topic, at the moment each answer is due?
            hit = stands_for[guess] == b['topic']
            add('guess_right', hit, asked)
            for q in range(per_segment):
                add(f'guess_right_first_q{q}', hit, asked & (b['segment'] == 0) & (b['number'] == q))
                add(f'guess_right_later_q{q}', hit, asked & (b['segment'] > 0) & (b['number'] == q))
        if not with_routes or routes[0] is None:
            continue
        per_block = torch.stack([c.any(1).sum(-1).float() for c in routes], dim=0)            # (blocks, B)
        touched.append(per_block.mean(0)); loaded.append(size['always_loaded'] + per_block.sum(0) * size['per_expert'])
        changes.append(torch.stack([(c[:, 1:] != c[:, :-1]).any(-1).float().mean(1) for c in routes], dim=0).mean(0))
        in_use.append(torch.stack([c.sum(-1).float().mean(1) for c in routes], dim=0).mean(0))
        for i, c in enumerate(routes):
            usage_topic[i].index_add_(0, b['topic'].reshape(-1), c.reshape(-1, N).float())
            usage_kind[i].index_add_(0, b['kind'].reshape(-1), c.reshape(-1, N).float())
        count_topic += torch.bincount(b['topic'].reshape(-1), minlength=a.topics); count_kind += torch.bincount(b['kind'].reshape(-1), minlength=4)
        if fetch_tokens == 0:   # the first 64 conversations are enough for the paging count
            for c in [c[:64].cpu().numpy() for c in routes]:
                for conversation in c:
                    for m in fetch:
                        fetch[m] += fetches(conversation, m)
            fetch_tokens = 64 * (b['tokens'].shape[1] - 1)
    out = dict(accuracy=right / total, questions=total, **{name: (r / n if n else None) for name, (r, n) in by.items()})
    out.update({f'opening_question_{name}_confidence': (c / n if n else None) for name, (c, n) in sure.items()})
    if touched:
        ut, uk = usage_topic / count_topic[None, :, None], usage_kind / count_kind[None, :, None]
        out.update(
            experts_touched_per_block=float(torch.cat(touched).mean()), experts_in_use_per_token=float(torch.cat(in_use).mean()),
            parameters_loaded=float(torch.cat(loaded).mean()), share_of_parameters_loaded=float(torch.cat(loaded).mean()) / size['total'],
            route_changes_per_token=float(torch.cat(changes).mean()),
            # 1.0: a topic (or a kind of token) always uses the same k experts. k/N: it uses all of them equally.
            route_follows_topic=float(ut.topk(k, dim=-1).values.sum(-1).mean() / k), route_follows_token_kind=float(uk.topk(k, dim=-1).values.sum(-1).mean() / k),
            experts_fetched_per_token={f'keeping_{m}_per_block': n / fetch_tokens for m, n in fetch.items()})
    model.train()
    return out


def train_planner(model, task, a, device, gen):
    """The planner learns first, alone, from unlabelled conversations, and is then left unchanged."""
    planner = model.planner
    opt = torch.optim.Adam(planner.parameters(), lr=a.planner_lr)
    curve, started = [], time.perf_counter()
    for step in range(1, a.planner_steps + 1):
        for group in opt.param_groups:
            group['lr'] = a.planner_lr * (0.05 + 0.95 * 0.5 * (1 + math.cos(math.pi * step / a.planner_steps)))
        b = task.batch(a.batch, (16, 8, 4, 2)[int(torch.randint(4, (1,), generator=gen, device=device))], 'train', gen, device)
        predict, even = planner.loss(b['tokens'], a.window)
        loss = predict + a.even * even
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(planner.parameters(), 1.0)
        opt.step()
        if step % max(1, a.planner_steps // 5) == 0:
            curve.append(dict(step=step, predict=predict.item(), even=even.item()))
            print(f'planner step {step} predict {predict.item():.4f} even {even.item():.4f}', flush=True)
    planner.requires_grad_(False)
    stands_for, counts = guess_to_topic(model, task, a, device)
    used = counts.sum(1) / counts.sum()
    info = dict(seconds=round(time.perf_counter() - started, 1), curve=curve,
                guesses_in_use=int((used > 0.01).sum()), topics_with_a_guess=int(stands_for[used > 0.01].unique().numel()),
                tokens_whose_guess_fits_the_topic=float(counts.max(1).values.sum() / counts.sum()))
    print(f'planner: {info["guesses_in_use"]} of {a.guesses} guesses in use, standing for {info["topics_with_a_guess"]} of {a.topics} topics; '
          f'{100 * info["tokens_whose_guess_fits_the_topic"]:.1f}% of tokens in one-topic conversations get a guess that fits', flush=True)
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arm', required=True, choices=['dense', 'oracle', 'fast', 'sum', 'guess'])
    ap.add_argument('--out', default='')
    ap.add_argument('--device', default='cuda:0')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--data-seed', type=int, default=20261114)
    for name, value in dict(topics=16, first=512, own=32, second=64, attributes=6, values=64, width=128, heads=4, blocks=3, shared=64,
                            expert=128, experts=16, active=2, steps=24000, batch=256, warmup=300, log_every=2000,
                            guesses=16, planner_width=64, planner_steps=4000, window=8).items():
        ap.add_argument('--' + name.replace('_', '-'), type=int, default=value)
    ap.add_argument('--lr', type=float, default=1e-3)
    ap.add_argument('--balance', type=float, default=0.01)
    ap.add_argument('--decay', type=float, default=0.9)
    ap.add_argument('--planner-lr', type=float, default=3e-3)
    ap.add_argument('--even', type=float, default=1.0, help='how strongly the planner is pushed to use all its guesses equally often')
    ap.add_argument('--wide', type=float, default=0.0, help='guess arm: also switch on the experts of the second guess while the planner gives it at least this chance')
    ap.add_argument('--planner-only', action='store_true', help='train and score the planner, then stop')
    a = ap.parse_args()
    device = torch.device(a.device)
    torch.manual_seed(a.seed)
    task = Task(a, device)
    model = Net(a, task).to(device)
    size = sizes(a, model)
    gen = torch.Generator(device=device).manual_seed(1000 + a.seed)
    print(f'arm={a.arm} seed={a.seed} parameters={size["total"]} loaded_per_token={size["loaded_per_token"]} facts={task.distinct_facts} ({task.bits / 1e6:.2f} Mbit) '
          f'a name fits {task.topics_per_name:.2f} topics on average, {100 * task.names_with_one_topic:.0f}% fit only one | {torch.cuda.get_device_name(device)}', flush=True)
    planner = train_planner(model, task, a, device, gen) if a.arm == 'guess' else None
    if a.planner_only:
        for regime in REGIMES:
            r = evaluate(model, task, a, regime, device, conversations=1024)
            n = REGIMES[regime][0]
            print(f'{regime}: guess right {r["guess_right"]:.4f} | at the start, by question: ' + ' '.join(f'{r[f"guess_right_first_q{q}"]:.2f}' for q in range(min(n, 8)))
                  + (' | after a change: ' + ' '.join(f'{r[f"guess_right_later_q{q}"]:.2f}' for q in range(min(n, 8))) if r.get('guess_right_later_q0') is not None else '')
                  + f' | experts touched {r["experts_touched_per_block"]:.2f} per block, in use {r["experts_in_use_per_token"]:.2f} per token, route changes {r["route_changes_per_token"]:.3f} per token', flush=True)
        return
    opt = torch.optim.AdamW([p for p in model.parameters() if p.requires_grad], lr=a.lr, weight_decay=0.0)
    curve, started, loss_sum, seen = [], time.perf_counter(), 0.0, 0
    for step in range(1, a.steps + 1):
        rate = a.lr * (step / a.warmup if step <= a.warmup else 0.05 + 0.95 * 0.5 * (1 + math.cos(math.pi * (step - a.warmup) / max(1, a.steps - a.warmup))))
        for group in opt.param_groups:
            group['lr'] = rate
        b = task.batch(a.batch, (16, 8, 4, 2)[int(torch.randint(4, (1,), generator=gen, device=device))], 'train', gen, device)
        logits, balance, _, _ = model(b['tokens'], b['topic'])
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
    result = dict(arm=a.arm, seed=a.seed, config=vars(a), parameters=size,
                  facts=dict(distinct=task.distinct_facts, bits=task.bits, topics_per_name=task.topics_per_name, names_with_one_topic=task.names_with_one_topic,
                             best_first_question=task.best_first_question),
                  planner=planner, seconds=round(seconds, 1), torch=torch.__version__, device=torch.cuda.get_device_name(device), curve=curve,
                  regimes={name: evaluate(model, task, a, name, device) for name in REGIMES})
    for name, r in result['regimes'].items():
        n = REGIMES[name][0]
        extra = '' if 'share_of_parameters_loaded' not in r else f' | experts touched {r["experts_touched_per_block"]:.2f} per block, {100 * r["share_of_parameters_loaded"]:.1f}% of parameters'
        print(f'{name}: accuracy {r["accuracy"]:.4f} | by question at the start: ' + ' '.join(f'{r[f"first_q{q}"]:.2f}' for q in range(min(n, 6))) + extra, flush=True)
    if a.out:
        Path(a.out).parent.mkdir(parents=True, exist_ok=True)
        Path(a.out).write_text(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
