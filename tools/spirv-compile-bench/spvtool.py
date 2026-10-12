#!/usr/bin/env python3
"""Minimal SPIR-V module model and the experimental rewrites used by spirv-compile-bench.

This is an offline measurement tool, not part of the emulator.  It parses a SPIR-V binary into
functions / blocks, knows which operand words are ids (from the Khronos core grammar JSON), and
implements the compile-time experiments the council asked for:

  guards  rewrite "if (idx < len) { access }" triangles that the emitter generates around every
          buffer and LDS access, in one of two modes
            remove  the guard and its branch disappear (access runs unconditionally)
            select  the guard becomes data: the index is clamped to a dummy slot and the loaded
                    value is OpSelect-ed, so the access is branch free but keeps GCN semantics
  stats   words / blocks / phis / selects / bounds checks

Everything is plain stdlib python.  Run `python3 spvtool.py --help`.
"""
import argparse
import collections
import json
import os
import struct
import sys

# ---- opcodes used by the rewrites ---------------------------------------------------------------
OP_TYPE_BOOL, OP_TYPE_INT, OP_TYPE_ARRAY, OP_TYPE_POINTER = 20, 21, 28, 32
OP_CONSTANT = 43
OP_FUNCTION, OP_FUNCTION_END, OP_VARIABLE = 54, 56, 59
OP_LOAD, OP_STORE, OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN = 61, 62, 65, 66
OP_ARRAY_LENGTH = 68
OP_PHI, OP_LOOP_MERGE, OP_SELECTION_MERGE, OP_LABEL = 245, 246, 247, 248
OP_BRANCH, OP_BRANCH_CONDITIONAL, OP_SWITCH = 249, 250, 251
OP_RETURN, OP_KILL, OP_UNREACHABLE, OP_RETURN_VALUE = 253, 252, 255, 254
OP_SELECT, OP_U_LESS_THAN = 169, 176
OP_NAME, OP_MEMBER_NAME = 5, 6
OP_DECORATE, OP_MEMBER_DECORATE = 71, 72
SC_WORKGROUP, SC_STORAGE_BUFFER = 4, 12
class Inst:
    __slots__ = ("op", "ops")

    def __init__(self, op, ops):
        self.op = op
        self.ops = ops  # operand words (everything after the opcode word)

    def words(self):
        return [((len(self.ops) + 1) << 16) | self.op] + self.ops


class Block:
    __slots__ = ("label", "insts")

    def __init__(self, label, insts):
        self.label = label  # block id
        self.insts = insts  # instructions after the OpLabel, terminator last


class Function:
    def __init__(self, head):
        self.head = head
        self.params = []
        self.blocks = []
        self.end = None


class Grammar:
    """Operand layout of every opcode, enough to find which words are ids."""

    def __init__(self, path):
        with open(path) as f:
            g = json.load(f)
        self.inst = {i["opcode"]: i for i in g["instructions"]}
        self.kinds = {k["kind"]: k for k in g["operand_kinds"]}
        self._cache = {}

    ID_KINDS = {"IdRef", "IdScope", "IdMemorySemantics", "IdResultType", "IdResult"}

    def _enum_params(self, kind, value):
        k = self.kinds[kind]
        out = []
        if k["category"] == "BitEnum":
            for e in sorted(k["enumerants"], key=lambda e: int(str(e["value"]), 0)):
                v = int(str(e["value"]), 0)
                if v and value & v == v:
                    out += e.get("parameters", [])
        else:
            for e in k["enumerants"]:
                if int(str(e["value"]), 0) == value:
                    out = e.get("parameters", [])
                    break
        return out

    def id_positions(self, op, ops):
        """Indices into `ops` that hold ids, and the index of the result id (or None)."""
        desc = self.inst.get(op)
        if desc is None:
            return [], None
        ids, result, i, n = [], None, 0, len(ops)

        def consume(kind):
            nonlocal i, result
            if i >= n:
                return
            if kind in self.ID_KINDS:
                if kind == "IdResult":
                    result = i
                ids.append(i)
                i += 1
            elif kind == "LiteralString":
                while i < n:
                    w = ops[i]
                    i += 1
                    if (w & 0xFF) == 0 or (w & 0xFF00) == 0 or (w & 0xFF0000) == 0 or (w >> 24) == 0:
                        break
            elif kind == "LiteralContextDependentNumber":
                i = n
            elif kind == "PairIdRefIdRef":
                ids.extend([i, i + 1]) if i + 1 < n else None
                i += 2
            elif kind == "PairIdRefLiteralInteger":
                ids.append(i)
                i += 2
            elif kind == "PairLiteralIntegerIdRef":
                if i + 1 < n:
                    ids.append(i + 1)
                i += 2
            else:
                k = self.kinds.get(kind)
                if k and k["category"] in ("BitEnum", "ValueEnum"):
                    v = ops[i]
                    i += 1
                    for p in self._enum_params(kind, v):
                        consume(p["kind"])
                else:
                    i += 1  # literal

        for d in desc.get("operands", []):
            q = d.get("quantifier", "")
            if q == "*":
                while i < n:
                    before = i
                    consume(d["kind"])
                    if i == before:
                        break
            else:
                consume(d["kind"])
        return ids, result


class Module:
    def __init__(self, words):
        if words[0] != 0x07230203:
            raise ValueError("not a SPIR-V binary (little endian words expected)")
        self.header = list(words[:5])
        self.globals = []  # instructions before the first OpFunction
        self.functions = []
        cur_fn, cur_block, i, n = None, None, 5, len(words)
        while i < n:
            wc, op = words[i] >> 16, words[i] & 0xFFFF
            inst = Inst(op, list(words[i + 1:i + wc]))
            i += wc
            if op == OP_FUNCTION:
                cur_fn = Function(inst)
                self.functions.append(cur_fn)
            elif cur_fn is None:
                self.globals.append(inst)
            elif op == OP_FUNCTION_END:
                cur_fn.end = inst
                cur_fn, cur_block = None, None
            elif op == OP_LABEL:
                cur_block = Block(inst.ops[0], [])
                cur_fn.blocks.append(cur_block)
            elif cur_block is None:
                cur_fn.params.append(inst)  # OpFunctionParameter
            else:
                cur_block.insts.append(inst)

    @property
    def bound(self):
        return self.header[3]

    def new_id(self):
        self.header[3] += 1
        return self.header[3] - 1

    def instructions(self):
        yield from self.globals
        for f in self.functions:
            yield f.head
            yield from f.params
            for b in f.blocks:
                yield Inst(OP_LABEL, [b.label])
                yield from b.insts
            yield f.end

    def to_words(self):
        out = list(self.header)
        for inst in self.instructions():
            out.extend(inst.words())
        return out

    def to_bytes(self):
        w = self.to_words()
        return struct.pack("<%dI" % len(w), *w)

    # -- statistics --------------------------------------------------------------------------------
    def stats(self):
        c = collections.Counter()
        words = len(self.header)
        for inst in self.instructions():
            words += len(inst.ops) + 1
            c[inst.op] += 1
        return {
            "words": words,
            "blocks": c[OP_LABEL],
            "phi": c[OP_PHI],
            "select": c[OP_SELECT],
            "cond_branch": c[OP_BRANCH_CONDITIONAL],
            "array_length": c[OP_ARRAY_LENGTH],
            "atomics": sum(v for k, v in c.items() if 227 <= k <= 242),
            "insts": sum(c.values()),
        }


def read_module(path):
    with open(path, "rb") as f:
        data = f.read()
    return Module(struct.unpack("<%dI" % (len(data) // 4), data))


# ---- guard rewrite ---------------------------------------------------------------------------------
# Instructions that are safe to execute speculatively (no side effects, independent of the set of
# active lanes): conversions, arithmetic, comparisons, selects, bit ops, composites, OpExtInst.
PURE_OPS = set(range(109, 206)) | {12, 77, 78, 79, 80, 81, 82, 83, 400}
ACCESS_OPS = {OP_LOAD, OP_STORE}
CHAIN_OPS = {OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN}
# what the dead-code step may delete when it becomes unused (never loads, stores or calls)
DEAD_REMOVABLE = (PURE_OPS - ACCESS_OPS) | CHAIN_OPS | {OP_ARRAY_LENGTH}


def succs(term):
    if term.op == OP_BRANCH:
        return [term.ops[0]]
    if term.op == OP_BRANCH_CONDITIONAL:
        return [term.ops[1], term.ops[2]]
    if term.op == OP_SWITCH:
        return [term.ops[1]] + term.ops[3::2]
    return []


class GuardRewriter:
    """Rewrites the emitter's bounds-check triangles.

    Shape (one per buffer / LDS access, 830 + ~600 times in the biggest compute shader):

        H:  %c = OpULessThan %idx %len          ; len = OpArrayLength(buffer) or an LDS constant
            OpSelectionMerge %M
            OpBranchConditional %c %T %M
        T:  %p = OpAccessChain ...; %v = OpLoad %p   (or OpStore)  ; OpBranch %M
        M:  %r = OpPhi %v %T %zero %H ...

    remove  -> T and M are folded into H; phis take the value from T.  Out-of-bounds is then left
               to the hardware (VK_EXT_robustness2: loads return 0, stores are dropped).
    select  -> same folding, but the index becomes `%c ? %idx : %len` (the array has one extra
               dummy slot at index len) and each phi becomes `OpSelect %c %v %zero`.  Loads and
               stores keep the GCN behaviour (OOB load = 0, OOB store dropped) with no branch.
    """

    def __init__(self, module, grammar):
        self.m = module
        self.g = grammar
        self.stats = collections.Counter()
        self.def_inst = {}
        for inst in self.m.globals:
            r = self._result(inst)
            if r is not None:
                self.def_inst[r] = inst
        self._dummy_arrays = {}
        self.buffer_mode = self.lds_mode = None

    def _result(self, inst):
        _, res = self.g.id_positions(inst.op, inst.ops)
        return inst.ops[res] if res is not None else None

    def rewrite(self, buffer_mode=None, lds_mode=None):
        """modes: None (leave), 'remove', or 'select' (LDS only)."""
        self.buffer_mode, self.lds_mode = buffer_mode, lds_mode
        roots = []
        for fn in self.m.functions:
            self._rewrite_function(fn, roots)
        self._dce(roots)
        return self.stats

    # -- guard recognition -------------------------------------------------------------------------
    def _classify_guard(self, cond_def, local_defs):
        if cond_def is None or cond_def.op != OP_U_LESS_THAN:
            return None
        idx, ln = cond_def.ops[2], cond_def.ops[3]
        ln_def = local_defs.get(ln) or self.def_inst.get(ln)
        if ln_def is None:
            return None
        if ln_def.op == OP_ARRAY_LENGTH:
            return "buffer", idx, ln
        if ln_def.op == OP_CONSTANT:
            return "const", idx, ln
        return None

    def _chain_storage_class(self, chain):
        t = self.def_inst.get(chain.ops[0])
        return t.ops[1] if t is not None and t.op == OP_TYPE_POINTER else None

    # -- per function ------------------------------------------------------------------------------
    def _rewrite_function(self, fn, roots):
        local_defs = {}
        for b in fn.blocks:
            for inst in b.insts:
                r = self._result(inst)
                if r is not None:
                    local_defs[r] = inst
        by_label = {b.label: b for b in fn.blocks}
        preds = collections.defaultdict(set)
        merge_refs = collections.defaultdict(set)  # merge/continue target -> header labels naming it
        for b in fn.blocks:
            for s in succs(b.insts[-1]):
                preds[s].add(b.label)
            for inst in b.insts:
                if inst.op == OP_SELECTION_MERGE:
                    merge_refs[inst.ops[0]].add(b.label)
                elif inst.op == OP_LOOP_MERGE:
                    merge_refs[inst.ops[0]].add(b.label)
                    merge_refs[inst.ops[1]].add(b.label)
        ctx = dict(by_label=by_label, preds=preds, merge_refs=merge_refs, local_defs=local_defs,
                   subst={}, rename={}, dead=set(), roots=roots)
        for h in fn.blocks:
            if h.label in ctx["dead"]:
                continue
            while self._fold_triangle(h, ctx):
                pass
        if ctx["dead"]:
            fn.blocks = [b for b in fn.blocks if b.label not in ctx["dead"]]
            self._apply_renames(fn, ctx["rename"], ctx["subst"])

    def _fold_triangle(self, h, ctx):
        insts = h.insts
        if len(insts) < 3 or insts[-1].op != OP_BRANCH_CONDITIONAL or insts[-2].op != OP_SELECTION_MERGE:
            return False
        cond, t_id, m_id = insts[-1].ops[:3]
        local_defs, preds = ctx["local_defs"], ctx["preds"]
        guard = self._classify_guard(local_defs.get(cond), local_defs)
        if guard is None:
            return False  # lane (EXEC) predicates and every other branch stay untouched
        st = self.stats
        t, m = ctx["by_label"].get(t_id), ctx["by_label"].get(m_id)
        if insts[-2].ops[0] != m_id or t is None or m is None or t_id == m_id:
            st["skip_not_triangle"] += 1
            return False
        if (preds[t_id] != {h.label} or preds[m_id] != {h.label, t_id}
                or ctx["merge_refs"][m_id] != {h.label} or ctx["merge_refs"][t_id]):
            st["skip_preds"] += 1
            return False
        if t.insts[-1].op != OP_BRANCH or t.insts[-1].ops[0] != m_id:
            st["skip_body_not_straight"] += 1
            return False
        body = t.insts[:-1]
        if not any(i.op in ACCESS_OPS for i in body):
            st["skip_no_access"] += 1
            return False
        if any(i.op not in PURE_OPS and i.op not in ACCESS_OPS and i.op not in CHAIN_OPS for i in body):
            st["skip_body_effects"] += 1
            return False
        chains = [i for i in body if i.op in CHAIN_OPS]
        sc = self._chain_storage_class(chains[0]) if chains else None
        kind = guard[0]
        if kind == "const":
            kind = "lds" if sc == SC_WORKGROUP else None
        elif sc != SC_STORAGE_BUFFER:
            kind = None
        if kind is None:
            st["skip_unknown_guard_kind"] += 1
            return False
        mode = self.buffer_mode if kind == "buffer" else self.lds_mode
        if mode is None:
            return False
        phis, k = [], 0
        while k < len(m.insts) and m.insts[k].op == OP_PHI:
            phis.append(m.insts[k])
            k += 1
        plan = []
        for p in phis:
            pairs = {self._root(p.ops[j + 1], ctx["rename"]): p.ops[j] for j in range(2, len(p.ops), 2)}
            if set(pairs) != {h.label, t_id}:
                st["skip_phi_shape"] += 1
                return False
            plan.append((p, pairs[t_id], pairs[h.label]))
        if mode == "select" and (kind != "lds" or not self._prepare_dummy_slot(chains, guard)):
            st["skip_select_unsupported"] += 1
            return False

        new = insts[:-2]
        if mode == "remove":
            for p, v_t, _ in plan:
                ctx["subst"][p.ops[1]] = v_t
        else:
            idx, ln = guard[1], guard[2]
            idx_def = local_defs.get(idx) or self.def_inst.get(idx)
            sel_id = self.m.new_id()
            new.append(Inst(OP_SELECT, [idx_def.ops[0], sel_id, cond, idx, ln]))
            for c in chains:
                c.ops = [sel_id if (j >= 3 and o == idx) else o for j, o in enumerate(c.ops)]
        new.extend(body)
        if mode == "select":
            new.extend(Inst(OP_SELECT, [p.ops[0], p.ops[1], cond, v_t, v_h]) for p, v_t, v_h in plan)
        new.extend(m.insts[k:])
        h.insts = new
        for i in new:
            r = self._result(i)
            if r is not None:
                local_defs[r] = i
        # M's terminator now belongs to H: fix predecessor and merge bookkeeping
        for s in succs(new[-1]):
            preds[s] = {h.label if x == m_id else x for x in preds[s]}
        for refs in ctx["merge_refs"].values():
            if m_id in refs:
                refs.discard(m_id)
                refs.add(h.label)
        ctx["rename"][m_id] = h.label
        ctx["dead"].update((t_id, m_id))
        ctx["roots"].append(cond)
        st["guards_%s_%s" % (kind, mode)] += 1
        return True

    def _prepare_dummy_slot(self, chains, guard):
        """Grow the LDS array by one element so that index `len` is a harmless dummy slot."""
        var = self.def_inst.get(chains[0].ops[2]) if chains else None
        if var is None or var.op != OP_VARIABLE or var.ops[2] != SC_WORKGROUP:
            return False
        ptr = self.def_inst.get(var.ops[0])
        arr = self.def_inst.get(ptr.ops[2]) if ptr is not None else None
        if arr is None or arr.op != OP_TYPE_ARRAY:
            return False  # block-wrapped / int64-aliased LDS
        if arr.ops[0] in self._dummy_arrays:
            return self._dummy_arrays[arr.ops[0]] == guard[2]  # same original length constant?
        if arr.ops[2] != guard[2]:
            return False  # array length is not the guard's constant
        if True:  # (single allocation block)
            old = self.def_inst[arr.ops[2]]
            new_const = self.m.new_id()
            const = Inst(OP_CONSTANT, [old.ops[0], new_const, old.ops[2] + 1])
            self.m.globals.insert(self.m.globals.index(arr), const)
            self.def_inst[new_const] = const
            arr.ops[2] = new_const
            self._dummy_arrays[arr.ops[0]] = old.ops[1]  # original length constant id
        return True

    @staticmethod
    def _root(x, table):
        while x in table:
            x = table[x]
        return x

    def _apply_renames(self, fn, rename, subst):
        root = self._root

        for b in fn.blocks:
            for inst in b.insts:
                if inst.op == OP_PHI:
                    for j in range(3, len(inst.ops), 2):
                        inst.ops[j] = root(inst.ops[j], rename)
                if subst:
                    ids, _ = self.g.id_positions(inst.op, inst.ops)
                    for pos in ids:
                        if inst.ops[pos] in subst:
                            inst.ops[pos] = root(inst.ops[pos], subst)

    # -- dead code that only the removed guards kept alive ----------------------------------------------
    def _dce(self, roots):
        g, uses, defs = self.g, collections.Counter(), {}
        for inst in self.m.instructions():
            ids, res = g.id_positions(inst.op, inst.ops)
            for pos in ids:
                if pos != res:
                    uses[inst.ops[pos]] += 1
            if res is not None:
                defs[inst.ops[res]] = inst
        work, killed = list(roots), set()
        while work:
            i = work.pop()
            d = defs.get(i)
            if d is None or uses[i] != 0 or i in killed or d.op not in DEAD_REMOVABLE:
                continue
            killed.add(i)
            ids, res = g.id_positions(d.op, d.ops)
            for pos in ids:
                if pos != res:
                    uses[d.ops[pos]] -= 1
                    work.append(d.ops[pos])
        if killed:
            for fn in self.m.functions:
                for b in fn.blocks:
                    b.insts = [x for x in b.insts if self._result(x) not in killed]
            self.m.globals = [x for x in self.m.globals
                              if not (x.op in (OP_NAME, OP_DECORATE) and x.ops[0] in killed)]
            self.stats["dead_removed"] += len(killed)


def apply_variant(module, grammar, name):
    """Variants that are pure SPIR-V rewrites (spirv-opt variants are done by run_matrix)."""
    rw = GuardRewriter(module, grammar)
    if name == "nobounds":      # diagnostic: every bounds check if removed (buffer and LDS)
        rw.rewrite(buffer_mode="remove", lds_mode="remove")
    elif name == "robust":      # buffer checks left to robustBufferAccess2
        rw.rewrite(buffer_mode="remove")
    elif name == "ldsclamp":    # LDS checks as clamp + select, no branch
        rw.rewrite(lds_mode="select")
    elif name == "robust+ldsclamp":
        rw.rewrite(buffer_mode="remove", lds_mode="select")
    elif name != "asis":
        raise ValueError(name)
    return rw.stats


def default_grammar_path():
    here = os.path.dirname(os.path.abspath(__file__))
    cands = [os.path.join(here, "..", "..", "3rdparty", "SPIRV-Headers", "include", "spirv", "unified1",
                          "spirv.core.grammar.json"),
             "/home/claudio/Documents/ps5-emulator/kyty/src/3rdparty/SPIRV-Headers/include/spirv/unified1/"
             "spirv.core.grammar.json"]
    for c in cands:
        if os.path.exists(c):
            return c
    raise SystemExit("spirv.core.grammar.json not found; pass --grammar")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("--grammar", default=None)
    ap.add_argument("--variant", default="asis",
                    choices=["asis", "nobounds", "robust", "ldsclamp", "robust+ldsclamp"])
    ap.add_argument("-o", "--output")
    a = ap.parse_args()
    gr = Grammar(a.grammar or default_grammar_path())
    m = read_module(a.input)
    before = m.stats()
    st = apply_variant(m, gr, a.variant)
    after = m.stats()
    print(json.dumps({"before": before, "after": after, "rewrite": dict(st)}))
    if a.output:
        with open(a.output, "wb") as f:
            f.write(m.to_bytes())


if __name__ == "__main__":
    main()
