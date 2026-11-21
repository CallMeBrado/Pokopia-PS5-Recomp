from pathlib import Path
from PIL import Image, ImageChops

reference = Path('../../frame-reference-655360/loading-replay')
for draw in (3, 4):
    path = Path(f'after-draw{draw}.rgba')
    rendered = Image.frombytes('RGBA', (1920, 1080), path.read_bytes())
    expected = Image.open(reference / f'after-draw{draw}.png').convert('RGBA')
    rgb = rendered.convert('RGB')
    delta = ImageChops.difference(expected.convert('RGB'), rgb)
    histogram = delta.histogram()
    nonzero_channels = sum(histogram[1:256]) + sum(histogram[257:512]) + sum(histogram[513:768])
    assert rgb.getbbox() == expected.convert('RGB').getbbox()
    print(f'draw {draw}: rgb delta {delta.getextrema()}, nonzero channels {nonzero_channels}, visible bounds {rgb.getbbox()}')
    assert max(hi for _, hi in delta.getextrema()) <= 1
    if draw == 4:
        rendered.save('rendered-ditto.png')
