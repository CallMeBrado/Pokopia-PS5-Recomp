from pathlib import Path
p=Path('vk_adapter.c')
s=p.read_text()
s=s.replace('linear_format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;',
            '(linear_format == VK_FORMAT_R16G16B16A16_SFLOAT || linear_format == VK_FORMAT_R32G32_SFLOAT) ? 8 : 4;')
s=s.replace('         linear_format != VK_FORMAT_R32_SFLOAT && linear_format != VK_FORMAT_R32G32B32A32_SFLOAT &&',
            '         linear_format != VK_FORMAT_R32_SFLOAT && linear_format != VK_FORMAT_R32G32_SFLOAT &&\n'
            '         linear_format != VK_FORMAT_R32G32B32A32_SFLOAT &&')
p.write_text(s)
p=Path('texture_upload.c')
s=p.read_text()
s=s.replace('    case VK_FORMAT_R32G32B32A32_SFLOAT:', '''    case VK_FORMAT_R32G32_SFLOAT:
        result.block_extent = (VkExtent2D){1, 1};
        result.bytes_per_block = 8;
        break;
    case VK_FORMAT_R32G32B32A32_SFLOAT:''')
p.write_text(s)
