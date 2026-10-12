#!/usr/bin/env python3
"""No-GPU checks for the SPIR-V rewrites in spvtool.py (run by ctest, or directly)."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import spvtool

HERE = os.path.dirname(os.path.abspath(__file__))


def grammar():
    path = os.environ.get("KYTY_SPIRV_GRAMMAR") or spvtool.default_grammar_path()
    return spvtool.Grammar(path)


@unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"), "SPIRV-Tools CLI not installed")
class GuardRewrites(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="spvtool-test-")
        cls.src = os.path.join(cls.dir, "guards.spv")
        subprocess.check_call(["spirv-as", "--target-env", "spv1.3",
                               os.path.join(HERE, "tests", "guards.spvasm"), "-o", cls.src])
        cls.g = grammar()

    def variant(self, name):
        m = spvtool.read_module(self.src)
        stats = spvtool.apply_variant(m, self.g, name)
        out = os.path.join(self.dir, name + ".spv")
        with open(out, "wb") as f:
            f.write(m.to_bytes())
        val = subprocess.run(["spirv-val", "--target-env", "vulkan1.3", out], capture_output=True, text=True)
        self.assertEqual(val.returncode, 0, val.stderr)
        return m, dict(stats)

    def test_roundtrip_is_byte_identical(self):
        with open(self.src, "rb") as f:
            data = f.read()
        self.assertEqual(spvtool.read_module(self.src).to_bytes(), data)

    def test_nobounds_folds_every_guard(self):
        m, st = self.variant("nobounds")
        self.assertEqual(st["guards_buffer_remove"], 4)
        self.assertEqual(st["guards_lds_remove"], 2)
        self.assertEqual(m.stats()["blocks"], 3)  # entry, L0 (everything folded), return block merged
        self.assertEqual(m.stats()["phi"], 0)
        self.assertEqual(m.stats()["array_length"], 0)  # dead ArrayLength instructions removed

    def test_robust_leaves_lds_guards(self):
        m, st = self.variant("robust")
        self.assertEqual(st["guards_buffer_remove"], 4)
        self.assertNotIn("guards_lds_remove", st)
        self.assertEqual(m.stats()["phi"], 1)  # the LDS load phi stays

    def test_ldsclamp_is_branch_free_and_grows_lds_by_one(self):
        m, st = self.variant("ldsclamp")
        self.assertEqual(st["guards_lds_select"], 2)
        sizes = [i.ops for i in m.globals if i.op == spvtool.OP_CONSTANT and i.ops[2] == 17]
        self.assertEqual(len(sizes), 1, "LDS length constant 16 -> 17 (dummy slot)")
        # the LDS phi became an OpSelect: only the buffer-load phi remains
        self.assertEqual(m.stats()["phi"], 1)

    def test_combination(self):
        m, st = self.variant("robust+ldsclamp")
        self.assertEqual(m.stats()["phi"], 0)
        self.assertEqual(m.stats()["cond_branch"], 0)

    def test_grammar_id_positions(self):
        ids, res = self.g.id_positions(spvtool.OP_SELECT, [1, 2, 3, 4, 5])
        self.assertEqual((ids, res), ([0, 1, 2, 3, 4], 1))
        ids, res = self.g.id_positions(spvtool.OP_PHI, [1, 2, 10, 11, 12, 13])
        self.assertEqual(ids, [0, 1, 2, 3, 4, 5])
        # OpDecorate target 7 DescriptorSet 0: only the target is an id
        ids, _ = self.g.id_positions(spvtool.OP_DECORATE, [7, 34, 0])
        self.assertEqual(ids, [0])


if __name__ == "__main__":
    unittest.main()
