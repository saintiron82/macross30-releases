"""Shared configure/cache dependencies for the supported SPU generation paths.

SDK build_spu_workloads -> pipeline -> selected lifter is a CLI edge; the
reviewed Macross wrapper invokes SDK spu_lifter by CLI. Those entry points are
explicit. Local Python imports (including conditional imports) are followed by
AST, without executing generators. Literal Python filenames also cover current
subprocess targets. Unknown external wrappers remain uncacheable.
"""
import ast
from pathlib import Path
import re


def known_cli_target(tree, call, name):
    """Fail closed if a reviewed Python subprocess target becomes computed."""
    if not call.args:
        return False
    command = call.args[0]
    if name in ('build_spu_workloads.py', 'spu_program_pipeline.py'):
        variable = 'cmd' if name == 'build_spu_workloads.py' else 'command'
        if not isinstance(command, ast.Name) or command.id != variable:
            return False
        assignments = [node.value for node in ast.walk(tree) if isinstance(node, ast.Assign)
                       and any(isinstance(t, ast.Name) and t.id == variable for t in node.targets)]
        return (len(assignments) == 1 and isinstance(assignments[0], ast.List)
                and len(assignments[0].elts) >= 2
                and ast.dump(assignments[0].elts[1]) == ast.dump(ast.parse('args.lifter', mode='eval').body))
    if name == 'spu_lifter_wrapper.py':
        if not isinstance(command, ast.List) or len(command.elts) < 4:
            return False
        assignments = [node.value for node in ast.walk(tree) if isinstance(node, ast.Assign)
                       and any(isinstance(t, ast.Name) and t.id == 'tool' for t in node.targets)]
        expected = ast.parse("Path(__file__).resolve().parents[2] / 'vendor/ps3recomp-master/tools/spu_lifter.py'", mode='eval').body
        return (ast.dump(command.elts[3]) == ast.dump(ast.parse('str(tool)', mode='eval').body)
                and len(assignments) == 1 and ast.dump(assignments[0]) == ast.dump(expected))
    return False


def generator_dependencies(lifter, tools=None):
    tools = Path(tools or Path(__file__).resolve().parent).resolve()
    lifter = Path(lifter).resolve()
    roots = [tools]
    seeds = [tools / 'build_spu_workloads.py', tools / 'spu_lifter.py',
             tools / 'spu_generator_dependencies.py', lifter]
    if lifter != tools / 'spu_lifter.py':
        if lifter.name != 'spu_lifter_wrapper.py' or lifter.parent == tools:
            return None
        helper = lifter.parents[2] / 'tools/audit_spu_static_entries.py'
        if not helper.is_file():
            return None
        roots = [lifter.parent, helper.parent, tools]
        seeds.append(helper)
    pending, found = list(seeds), set()
    while pending:
        path = pending.pop().resolve()
        if path in found:
            continue
        if not path.is_file():
            raise ValueError('Missing SPU generator dependency: ' + str(path))
        found.add(path)
        tree = ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
        docstrings = {id(node.body[0].value) for node in ast.walk(tree)
                      if isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))
                      and node.body and isinstance(node.body[0], ast.Expr)
                      and isinstance(node.body[0].value, ast.Constant)
                      and isinstance(node.body[0].value.value, str)}
        names = set()
        for node in ast.walk(tree):
            # Do not silently promise a closure for patterns not used by the
            # reviewed generators. Unknown dynamic/package loading disables the
            # cache, and CMake conservatively tracks the SDK tool directory.
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == '__import__':
                return None
            if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                    and isinstance(node.func.value, ast.Name) and node.func.value.id == 'subprocess'
                    and node.func.attr in ('run', 'Popen', 'call', 'check_call', 'check_output')
                    and not known_cli_target(tree, node, path.name)):
                return None
            if isinstance(node, ast.Import):
                if any(alias.name.split('.')[0] == 'importlib' for alias in node.names):
                    return None
                names.update(alias.name.split('.')[0] + '.py' for alias in node.names)
            elif isinstance(node, ast.ImportFrom):
                if node.level or (node.module and node.module.split('.')[0] == 'importlib'):
                    return None
                if node.module:
                    names.add(node.module.split('.')[0] + '.py')
            elif isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in docstrings:
                names.update(re.findall(r'(?<![\w])([A-Za-z_]\w*\.py)(?![\w])', node.value))
        for name in names:
            for root in [path.parent, *roots]:
                candidate = root / name
                if candidate.is_file():
                    pending.append(candidate)
                    break
                if (root / name[:-3]).is_dir():
                    return None
    return sorted(found)


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--lifter', required=True)
    args = parser.parse_args()
    tools = Path(__file__).resolve().parent
    paths = generator_dependencies(args.lifter)
    if paths is None:
        # Unknown wrappers are never cached. Preserve conservative SDK tracking
        # for configure; no claim to discover that wrapper's external closure.
        paths = sorted(set(tools.glob('*.py')) | {Path(args.lifter).resolve()})
    print(';'.join(path.as_posix() for path in paths))


if __name__ == '__main__':
    main()
