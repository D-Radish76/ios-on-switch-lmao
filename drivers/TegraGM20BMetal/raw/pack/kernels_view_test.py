"""raw/pack/kernels.py: a kernel record that views a texture slot in texels of another size than its key binds there
is left out of the pack (the runtime would fail its command buffer: runtime/raw_view.h SlotView::None)."""

import unittest

import kernels

KEY = 'k | tg=8,8,1 | tgmem= | tex=0:70:2:1:1:02030405,1:115:2:1:1:02030405 | smp='


def tex(slot: int, window: int) -> dict:
    return {'off': 16 * slot, 'kind': kernels.TEXTURE, 'slot': slot, 'window': window}


class ViewsOtherTexelSize(unittest.TestCase):
    def test_same_size_views_stay(self) -> None:
        # RGBA8 viewed as A8B8G8R8 (0x08), RGBA16Float as R16_G16_B16_A16 (0x03)
        self.assertEqual(kernels.views_other_texel_size(KEY, [tex(0, 0x08), tex(1, 0x03)]), [])

    def test_another_size_is_named(self) -> None:
        # RGBA8 (4 bytes) viewed as R16_G16_B16_A16 (8 bytes)
        self.assertEqual(kernels.views_other_texel_size(KEY, [tex(0, 0x03), tex(1, 0x03)]), [0])

    def test_no_window_or_unknown_format_is_not_judged(self) -> None:
        self.assertEqual(kernels.views_other_texel_size(KEY, [tex(0, 0), tex(5, 0x1D)]), [])


if __name__ == '__main__':
    unittest.main()
