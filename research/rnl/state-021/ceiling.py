"""STATE-021 helper: the best any reader could score on each question, worked out from how the conversations are made.

A reader that knows every fact and every topic's syllables, but not which topic is meant, keeps the set of
topics that fit all the names so far and picks one of them. This counts how often that pick is right
(one-topic conversations, the same generator as the runs), and how often the value is then right.

  python ceiling.py
"""
import argparse, types
import torch
from run_find import Task

a = types.SimpleNamespace(topics=16, first=512, own=32, second=64, attributes=6, values=64, data_seed=20261114)
device = torch.device('cpu')
task = Task(a, device)
gen = torch.Generator().manual_seed(7)
b = task.batch(200000, 16, 'same', gen, device)
s1 = b['tokens'][:, 0::4]                                   # (n, 16) first syllables
topic = b['topic'][:, 0]
member = torch.zeros(a.topics, a.first, dtype=torch.bool)
for t in range(a.topics):
    member[t, task.own[t]] = True
alive = torch.ones(s1.shape[0], a.topics, dtype=torch.bool)
print(f'a name fits {task.topics_per_name:.2f} topics on average; {100 * task.names_with_one_topic:.1f}% of names fit only one')
print(f'best possible on the first question, from the formula: {100 * task.best_first_question:.1f}%')
for q in range(6):
    alive &= member[:, s1[:, q]].T                          # topics that still fit every name so far
    count = alive.sum(1).float()
    pick = 1.0 / count                                      # chance that a pick among them is the real topic
    best = pick + (1 - pick) / a.values
    print(f'question {q + 1}: topics still fitting {count.mean():.2f} on average; only one fits in {100 * (count == 1).float().mean():.1f}% of conversations; '
          f'best possible topic pick {100 * pick.mean():.1f}%, best possible answer {100 * best.mean():.1f}%')
