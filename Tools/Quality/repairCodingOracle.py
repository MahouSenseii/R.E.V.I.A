"""Run the frozen repair cohort's pure-function acceptance checks in a child process."""

import ast
import copy
import json
import random
import sys
from pathlib import Path


def load_function(source, name):
    if len(source) > 16384:
        raise ValueError("Source exceeds the executable oracle's bound")
    tree = ast.parse(source)
    if not tree.body or any(not isinstance(node, ast.FunctionDef) for node in tree.body):
        raise ValueError("Only pure function definitions are admitted")
    builtins = {
        "ValueError": ValueError, "TypeError": TypeError, "len": len, "range": range,
        "enumerate": enumerate, "sorted": sorted, "min": min, "max": max, "sum": sum,
        "list": list, "tuple": tuple, "dict": dict, "set": set, "int": int, "str": str,
        "bool": bool, "isinstance": isinstance, "zip": zip, "abs": abs,
    }
    names = set(builtins) | {node.name for node in tree.body}
    methods = {"append", "extend", "sort", "copy", "strip", "lower", "casefold", "get", "items", "values", "keys", "pop"}
    for node in ast.walk(tree):
        if isinstance(node, (ast.Import, ast.ImportFrom, ast.Global, ast.Nonlocal, ast.ClassDef, ast.AsyncFunctionDef)):
            raise ValueError("Source contains unsupported process or module access")
        if isinstance(node, ast.Attribute) and node.attr not in methods:
            raise ValueError("Source contains an unsupported method")
        if isinstance(node, ast.Name) and node.id.startswith("__"):
            raise ValueError("Source contains an unsupported special name")
        if isinstance(node, ast.FunctionDef) and (node.decorator_list or node.returns or any(arg.annotation for arg in node.args.args)):
            raise ValueError("Decorators and annotations are outside this pure-function oracle")
        if isinstance(node, ast.Call):
            if isinstance(node.func, ast.Name) and node.func.id in names:
                continue
            if isinstance(node.func, ast.Attribute) and node.func.attr in methods:
                continue
            raise ValueError("Source calls an unsupported function")
    namespace = {"__builtins__": builtins}
    exec(compile(tree, "<retained-model-code>", "exec"), namespace)
    if name not in namespace:
        raise ValueError("The requested function is absent")
    return namespace[name]


def interval_reference(ranges):
    covered = sorted({point for a, b in ranges for point in range(2 * a, 2 * b + 1)})
    result = []
    for point in covered:
        if not result or point > result[-1][1] + 1:
            result.append([point, point])
        else:
            result[-1][1] = point
    return [[a // 2, b // 2] for a, b in result]


def evaluate(source, oracle):
    checks = 0
    rng = random.Random(918273)
    if oracle == "merge-ranges-v1":
        function = load_function(source, "merge_ranges")
        cases = [[], [[1, 4]], [[1, 8], [2, 3]], [[1, 2], [2, 5]], [[1, 2], [3, 4]],
                 [[8, 12], [-7, -2], [-3, 0]], [[0, 0], [0, 0]], [[5, 7], [1, 6]]]
        for _ in range(100):
            cases.append([sorted([rng.randint(-20, 20), rng.randint(-20, 20)]) for _ in range(rng.randrange(12))])
        for values in cases:
            before = copy.deepcopy(values)
            expected = interval_reference(values)
            actual = function(values)
            assert actual == expected, ("interval result", before, expected, actual)
            assert values == before, ("input mutation", before, values)
            checks += 2
    elif oracle == "inventory-totals-v1":
        function = load_function(source, "inventory_totals")
        cases = [[], [(" Bolt ", 4), ("BOLT", -4)], [(" Nut", -3), ("nut ", 1)], [("A", 3), ("B", 5)]]
        for _ in range(100):
            cases.append([(rng.choice([" Bolt ", "BOLT", "nut", "NUT ", "Washer"]), rng.randint(-5, 5))
                          for _ in range(rng.randrange(20))])
        for values in cases:
            before = copy.deepcopy(values)
            names = {name.strip().lower() for name, _ in values}
            expected = {name: sum(quantity for key, quantity in values if key.strip().lower() == name) for name in names}
            expected = {name: total for name, total in expected.items() if total != 0}
            actual = function(values)
            assert actual == expected, ("inventory result", before, expected, actual)
            assert values == before, ("input mutation", before, values)
            checks += 2
        for values in [[("", 1)], [(" \t ", 0)], [("part", 2), (" ", -2)]]:
            try:
                function(values)
            except ValueError:
                checks += 1
            else:
                raise AssertionError("Empty names must raise ValueError")
    else:
        raise ValueError("Unknown frozen executable oracle")
    return checks


if __name__ == "__main__":
    try:
        if not __debug__:
            raise RuntimeError("Executable acceptance assertions must remain enabled")
        request = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
        checks = evaluate(request["code"], request["oracle"])
        print(json.dumps({"passed": True, "checks": checks}))
    except Exception as error:
        print(json.dumps({"passed": False, "error": str(error), "kind": type(error).__name__}))
        sys.exit(1)
