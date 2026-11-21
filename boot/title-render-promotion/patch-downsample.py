from pathlib import Path

path = Path('live_draw.c')
text = path.read_text()
old = '''    } else if (!strcmp(fragment, "c4664b38f6a87835")) {
        if (depth || target->format != VK_FORMAT_R8G8B8A8_SRGB || target->array_layers != 1 ||
            draw->color_write_mask != rgba || !supported_sample(&samples[0], target->format, true))
            return false;
'''
new = '''    } else if (!strcmp(fragment, "c4664b38f6a87835")) {
        /* The scene downsample samples a color texture with explicit NVN filtering.
         * The first title pass keeps a same-sized depth attachment bound with writes disabled. */
        const bool scene_downsample = depth && target->format == VK_FORMAT_R8G8B8A8_UNORM &&
            !draw->depth_write && supported_sample(&samples[0], VK_FORMAT_R8G8B8A8_SRGB, false);
        const bool color_downsample = !depth &&
            (target->format == VK_FORMAT_R8G8B8A8_SRGB || target->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) &&
            supported_sample(&samples[0], target->format, true);
        if (draw->target_layer || target->array_layers != 1 || draw->color_write_mask != rgba ||
            (!scene_downsample && !color_downsample))
            return false;
'''
assert text.count(old) == 1
path.write_text(text.replace(old,new))
