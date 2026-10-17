#!/usr/bin/env python3
"""
Permutation / cross-product representability tests.

Enumerates the cross-products of the combinable value state spaces and asserts
that every combination is representable: each constituent value resolves to a
defined Metal 4 target (through its owning register's disposition), and any
explicitly unrepresentable value is dispositioned with a fallback.

Each space is fully enumerated because every one is tractable (< 10 000):
`blend-factor-x-equation` 120, `texture-format-x-address-x-filter` 1080,
`depth-func-x-stencil-ops-x-two-sided` 8192, `surface-format-x-target-count` 64,
`primitive-x-cull-x-front-face` 40.  No pairwise reduction is required; the
tractability bound is asserted in `test_space_sizes_are_tractable`.
"""
from __future__ import annotations

import unittest

import _corpus


class TestPermutations(_corpus.DialTest):
    def test_permutation_representability(self):
        for space in _corpus.PERM_SPACES:
            for combo in _corpus.space_combinations(space):
                dial = f"{space.name}:" + "+".join(combo)
                self.assertContract(dial, _corpus.permutation_violations(space, combo))

    def test_members_exist_in_the_value_space(self):
        for space in _corpus.PERM_SPACES:
            self.assertContract(space.name, _corpus.space_member_violations(space))

    def test_space_sizes_are_tractable(self):
        for space in _corpus.PERM_SPACES:
            with self.subTest(space=space.name):
                self.assertLess(_corpus.space_size(space), 10_000)

    def test_owners_have_dispositions(self):
        for space in _corpus.PERM_SPACES:
            for dim, owner in space.owners.items():
                with self.subTest(space=space.name, dim=dim):
                    reg = _corpus.register_by_id(owner)
                    self.assertIn(reg.disposition, _corpus.DISPOSITIONS)


if __name__ == "__main__":
    unittest.main()
