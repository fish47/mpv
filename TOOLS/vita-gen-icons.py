#!/usr/bin/env python3

import io
import os
import re
import sys
import subprocess
import concurrent.futures
import xml.etree.ElementTree


def patch_svg(path: str) -> str:
    xml.etree.ElementTree.register_namespace('', 'http://www.w3.org/2000/svg')
    et = xml.etree.ElementTree.parse(path)
    # the default 'fill' value of <path> is black if not specified
    # we have to change it to white because of tinting
    for e in et.iterfind('{*}path'):
        e.attrib['fill'] = 'white'
    with io.BytesIO() as bio:
        et.write(bio)
        return bio.getvalue()


def export_png(in_path, out_path, size):
    svg_bytes = patch_svg(in_path)
    dimension = '%sx%s' % (size, size)
    cmd = ('magick', '-background', 'none', '-', '-resize', dimension, out_path)
    subprocess.run(cmd, input=svg_bytes)


def get_base_dir():
    abs_path = os.path.abspath(__file__)
    if os.path.islink(abs_path):
        abs_path = os.path.realpath(abs_path)
    base_dir = os.path.dirname(abs_path)
    base_dir = os.path.dirname(base_dir)
    return base_dir


def iterate_svg_files(svg_dir):
    if len(sys.argv) > 1:
        yield from iter(sys.argv[1:])
    else:
        def __normalize_path(p):
            return os.path.join(svg_dir, p)
        yield from iter(map(__normalize_path, os.listdir(svg_dir)))


def main():
    base_dir = get_base_dir()
    icon_png_dir = os.path.join(base_dir, 'etc/vita/icons')
    icon_svg_dir = os.path.join(base_dir, 'etc/vita/icons/svg')
    name_pattern = re.compile(r'(.*)_(\d+)')
    export_args = []
    for svg_path in iterate_svg_files(icon_svg_dir):
        if not os.path.isfile(svg_path):
            continue
        path_name = os.path.basename(svg_path)
        m = name_pattern.match(os.path.splitext(path_name)[0])
        if not m:
            continue
        icon_name, size = m.groups()
        png_path = os.path.join(icon_png_dir, icon_name + ".png")
        export_args.append((svg_path, png_path, size))
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
        executor.map(lambda args: export_png(*args), export_args)


if __name__ == "__main__":
    main()
